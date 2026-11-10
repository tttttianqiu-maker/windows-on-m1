/* SPDX-License-Identifier: MIT */
/*
 * Host test of m1n1's Broadcom FullMAC port (m1n1_windows/src/brcm_msgbuf.c + brcm_fwil.c)
 * against a simulated dongle. The dongle side is written from the Linux brcmfmac layouts
 * (pcie.c brcmf_pcie_dhi_ringinfo / ring descriptors, msgbuf.c message structs, fweh.h
 * event packets, fwil_types.h join/PMK structs) with its own byte offsets, not from the
 * driver's structs, so a layout or ordering slip in the driver shows up here.
 *
 * Covered: ring setup, hostready-before-posting, max_rxbufpost, ioctl round trips with ring
 * wrap-around, cache maintenance alignment, dongle init (MAC, CLM/TxCap chunking, events,
 * interface up), the WPA2-PSK join sequence and structs (PMK, then passphrase retry), the TX
 * flowring, RX completions (802.3 / flags 0 / 802.11), D2H mailbox data, link loss.
 */
#include "brcm_host_test.h"

#include "brcm.h"
#include "brcm_msgbuf.h"
#include "brcm_priv.h"

static int failures;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                    \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/* ---------- little-endian helpers (dongle side) ---------- */

static void p16(u8 *b, u32 o, u16 v) { b[o] = v; b[o + 1] = v >> 8; }
static void p32(u8 *b, u32 o, u32 v) { p16(b, o, v); p16(b, o + 2, v >> 16); }
static u16 g16(const u8 *b, u32 o) { return b[o] | (b[o + 1] << 8); }
static u32 g32(const u8 *b, u32 o) { return g16(b, o) | ((u32)g16(b, o + 2) << 16); }
static u64 g64(const u8 *b, u32 o) { return g32(b, o) | ((u64)g32(b, o + 4) << 32); }
static void pbe16(u8 *b, u32 o, u16 v) { b[o] = v >> 8; b[o + 1] = v; }
static void pbe32(u8 *b, u32 o, u32 v) { pbe16(b, o, v >> 16); pbe16(b, o + 2, v); }

/* ---------- m1n1 environment ---------- */

struct brcm brcm;
void *adt;

static u64 now_us;
u64 get_ticks(void) { return now_us++; }
u64 ticks_to_msecs(u64 t) { return t / 1000; }
u64 ticks_to_usecs(u64 t) { return t; }

static void dongle_service(void);
void udelay(u32 us)
{
    now_us += us;
    dongle_service();
}
void mdelay(u32 ms) { udelay(ms * 1000); }

static int unaligned_syncs;
void dc_civac_range(void *addr, size_t length)
{
    if (((uintptr_t)addr & 63) || (length & 63) || !length)
        unaligned_syncs++;
}

void *host_memalign(size_t align, size_t size)
{
    return aligned_alloc(align, ALIGN_UP(size, align));
}

u32 read32(u64 addr) { (void)addr; return 0; } /* DART error register: no fault */
void write32(u64 addr, u32 val) { (void)addr; (void)val; }

int adt_path_offset(const void *a, const char *path) { (void)a; (void)path; return -1; }
int adt_path_offset_trace(const void *a, const char *path, int *o) { (void)a; (void)path; (void)o; return -1; }
int adt_get_reg(const void *a, int *p, const char *prop, int i, u64 *addr, u64 *size)
{
    (void)a; (void)p; (void)prop; (void)i; (void)addr; (void)size;
    return -1;
}
struct host_dart { int dummy; };
static struct host_dart the_dart;
static u8 *map_va;
static u64 map_iova, map_len;
dart_dev_t *dart_init_adt(const char *path, int instance, int device, bool keep_pts)
{
    (void)path; (void)instance; (void)device; (void)keep_pts;
    return NULL;
}
dart_dev_t *dart_init(uintptr_t base, u8 device, bool keep_pts, enum dart_type_t type)
{
    CHECK(base == 0x594000000UL && device == 1 && !keep_pts && type == DART_T8110);
    return &the_dart;
}
int dart_map(dart_dev_t *d, uintptr_t iova, void *bfr, size_t len)
{
    CHECK(d == &the_dart && !((uintptr_t)bfr & (SZ_16K - 1)));
    map_va = bfr;
    map_iova = iova;
    map_len = len;
    return 0;
}

/* ---------- TCM ---------- */

#define TCM_SIZE  0x10000
#define SHARED    0x1000
#define RINGINFO  0x2000
#define RINGMEM   0x3000
#define H2D_W     0x4000
#define H2D_R     0x4100
#define D2H_W     0x4200
#define D2H_R     0x4300
static u8 tcm[TCM_SIZE];

u32 tcm_read32(u32 off) { CHECK(off + 4 <= TCM_SIZE); return g32(tcm, off); }
void tcm_write32(u32 off, u32 v) { CHECK(off + 4 <= TCM_SIZE); p32(tcm, off, v); }
u16 tcm_read16(u32 off) { CHECK(off + 2 <= TCM_SIZE); return g16(tcm, off); }
void tcm_write16(u32 off, u16 v) { CHECK(off + 2 <= TCM_SIZE); p16(tcm, off, v); }
void brcm_console_poll(void) {}

static int doorbells, hostready, doorbells_before_hostready;
void pcie_write32(u32 reg, u32 val)
{
    if (reg == 0x140 || reg == 0xa20) { /* H2D_MAILBOX_0 */
        CHECK(val == 1);
        CHECK(reg == 0x140); /* no SHARED_DAR in this firmware */
        doorbells++;
        if (!hostready)
            doorbells_before_hostready++;
        dongle_service();
    } else if (reg == 0x144 || reg == 0xa24) { /* H2D_MAILBOX_1: hostready */
        hostready++;
    }
}

/* ---------- the dongle ---------- */

static u8 *host(u64 iova, u32 len)
{
    CHECK(map_va && iova >= map_iova && iova + len <= map_iova + map_len);
    if (!(map_va && iova >= map_iova && iova + len <= map_iova + map_len)) {
        static u8 junk[8192];
        return junk;
    }
    return map_va + (iova - map_iova);
}

struct dring {
    u64 iova;
    u16 depth, item, idx; /* idx: device read (H2D) or write (D2H) position */
};
static struct dring h2d[3]; /* control submit, rx post, TX flowring (id 2) */
static struct dring d2h[3]; /* control complete, TX complete, RX complete */

struct dbuf {
    u32 id;
    u64 addr;
    u16 len;
};
#define NBUF 512
static struct dbuf rx_free[NBUF], ev_free[NBUF], io_free[NBUF];
static int n_rx, n_ev, n_io;

static u8 fw_mac[6];
static const u8 ap_mac[6] = {0xa0, 0xb1, 0xc2, 0xd3, 0xe4, 0xf5};

/* firmware call log */
struct call {
    u32 cmd;
    char name[32];
    u8 data[1600];
    u32 len;
};
#define NCALL 1024
static struct call calls[NCALL];
static int ncalls;

/* fake firmware state */
static u8 clm_got[8192];
static u32 clm_len_got, clm_chunks, txcap_chunks;
static u16 clm_flags[8], txcap_flag;
static u8 event_mask[32];
static u32 event_mask_len;
static bool fw_is_up, sup_wpa, allmulti;
static u32 pm_mode = 99, infra = 99, wpa_auth, wsec, auth = 99, mfp = 99;
static u8 pmk_key[256];
static u16 pmk_len, pmk_flags;
static u8 good_pmk[32];
static const char good_pass[] = "correct horse battery";
static int joins, h2d_mb_ds_ack, flow_creates;
static u8 last_join[128];
static u32 last_join_len;

struct pend_event {
    u32 type, status, reason;
    u16 flags;
};
static struct pend_event evq[64];
static int evq_n;
static void queue_event(u32 type, u32 status, u32 reason, u16 flags)
{
    if (evq_n < 64)
        evq[evq_n++] = (struct pend_event){type, status, reason, flags};
}

static void d2h_put(int ring, const u8 *msg, u32 len)
{
    struct dring *r = &d2h[ring];
    CHECK(r->depth && len <= r->item);
    u16 host_r = tcm_read16(D2H_R + 4 * ring);
    CHECK((u16)((r->idx + 1) % r->depth) != host_r); /* ring full */
    u8 *item = host(r->iova + (u64)r->idx * r->item, r->item);
    memset(item, 0, r->item);
    memcpy(item, msg, len);
    r->idx = (r->idx + 1) % r->depth;
    tcm_write16(D2H_W + 4 * ring, r->idx);
}

static s16 fw_ioctl(u32 cmd, const u8 *in, u32 in_len, u32 out_len, u8 *resp, u32 *resp_len)
{
    struct call *c = &calls[ncalls < NCALL ? ncalls++ : NCALL - 1];
    memset(c, 0, sizeof(*c));
    c->cmd = cmd;
    const u8 *data = in;
    u32 dlen = in_len;
    if (cmd == 262 || cmd == 263) {
        u32 nl = 0;
        while (nl < in_len && in[nl])
            nl++;
        CHECK(nl < in_len && nl < sizeof(c->name));
        memcpy(c->name, in, nl);
        data = in + nl + 1;
        dlen = in_len - nl - 1;
    }
    c->len = min(dlen, (u32)sizeof(c->data));
    memcpy(c->data, data, c->len);
    *resp_len = 0;

    if (cmd == 262) { /* GET_VAR: the answer replaces the request */
        memset(resp, 0, out_len);
        *resp_len = out_len;
        if (!strcmp(c->name, "cur_etheraddr"))
            memcpy(resp, fw_mac, 6);
        else if (!strcmp(c->name, "ver"))
            strcpy((char *)resp, "wl0: Oct 10 2025 01:02:03 version 23.10.1040.4 FWID 01-0\nx");
        else if (!strcmp(c->name, "clmver"))
            strcpy((char *)resp, "API: 20.0 Data: Apple.J613");
        else if (!strcmp(c->name, "event_msgs_ext")) {
            CHECK(dlen == 36 && data[0] == 1 && data[1] == 0 && data[2] == 32 && data[3] == 32);
            resp[0] = 1;
            resp[2] = 27;
            resp[3] = 27;
            memset(resp + 4, 0xff, 27); /* the firmware's own default mask */
        } else if (!strcmp(c->name, "join_ver")) {
            p16(resp, 0, 1);
            p16(resp, 2, 8);
            p16(resp, 4, 1);
        } else if (!strcmp(c->name, "mfp"))
            p32(resp, 0, 0);
        else if (!strcmp(c->name, "sup_wpa"))
            p32(resp, 0, 1);
        else if (!strcmp(c->name, "mpc"))
            p32(resp, 0, 1);
        else if (!strcmp(c->name, "country")) {
            memcpy(resp, "XZ\0\0", 4);
            p32(resp, 4, 11);
            memcpy(resp + 8, "XZ\0\0", 4);
        } else {
            *resp_len = 0;
            return -23; /* BCME_UNSUPPORTED */
        }
        return 0;
    }
    if (cmd == 263) { /* SET_VAR */
        if (!strcmp(c->name, "cur_etheraddr")) {
            CHECK(dlen == 6);
            memcpy(fw_mac, data, 6);
        } else if (!strcmp(c->name, "clmload") || !strcmp(c->name, "txcapload")) {
            /* struct brcmf_dload_data_le: flag, dload_type, len, crc, data */
            u16 flag = g16(data, 0);
            CHECK(g16(data, 2) == 2); /* DL_TYPE_CLM for every blob */
            u32 n = g32(data, 4);
            CHECK(g32(data, 8) == 0 && n + 12 == dlen && n <= 1400);
            if (c->name[0] == 'c') {
                if (clm_chunks < 8)
                    clm_flags[clm_chunks] = flag;
                clm_chunks++;
                memcpy(clm_got + clm_len_got, data + 12, n);
                clm_len_got += n;
            } else {
                txcap_flag = flag;
                txcap_chunks++;
            }
        } else if (!strcmp(c->name, "event_msgs_ext")) {
            CHECK(data[0] == 1 && data[1] == 3 && dlen == 4u + data[2]);
            event_mask_len = data[2];
            memcpy(event_mask, data + 4, min(event_mask_len, 32u));
        } else if (!strcmp(c->name, "wpa_auth"))
            wpa_auth = g32(data, 0);
        else if (!strcmp(c->name, "wsec"))
            wsec = g32(data, 0);
        else if (!strcmp(c->name, "auth"))
            auth = g32(data, 0);
        else if (!strcmp(c->name, "mfp"))
            mfp = g32(data, 0);
        else if (!strcmp(c->name, "sup_wpa"))
            sup_wpa = g32(data, 0);
        else if (!strcmp(c->name, "allmulti"))
            allmulti = g32(data, 0);
        else if (!strcmp(c->name, "join")) {
            CHECK(fw_is_up && infra == 1 && sup_wpa && wpa_auth == 0x80 && wsec == 4 && !auth);
            joins++;
            last_join_len = min(dlen, (u32)sizeof(last_join));
            memcpy(last_join, data, last_join_len);
            bool key_ok = (pmk_flags == 0 && pmk_len == 32 && !memcmp(pmk_key, good_pmk, 32)) ||
                          (pmk_flags == 1 && pmk_len == strlen(good_pass) &&
                           !memcmp(pmk_key, good_pass, pmk_len));
            queue_event(BRCM_E_SET_SSID, 0, 0, 0);
            queue_event(BRCM_E_LINK, 0, 0, 1);
            queue_event(BRCM_E_PSK_SUP, key_ok ? 6 : 7, 0, 0);
        }
        return 0;
    }
    switch (cmd) {
        case 2: fw_is_up = true; return 0;          /* UP */
        case 86: pm_mode = g32(in, 0); return 0;    /* SET_PM */
        case 20: infra = g32(in, 0); return 0;      /* SET_INFRA */
        case 268:                                   /* SET_WSEC_PMK */
            CHECK(in_len == 262); /* brcmf_wsec_pmk_le: key_len, flags, key[256], opt_len */
            pmk_len = g16(in, 0);
            pmk_flags = g16(in, 2);
            memcpy(pmk_key, in + 4, 256);
            return 0;
        case 98: /* GET_REVINFO */
            memset(resp, 0, out_len);
            CHECK(out_len >= 68);
            p32(resp, 11 * 4, 0x4388);
            p32(resp, 3 * 4, 4);
            *resp_len = out_len;
            return 0;
        case 23: /* GET_BSSID */
            memcpy(resp, ap_mac, 6);
            *resp_len = 6;
            return 0;
        case 127: /* GET_RSSI */
            p32(resp, 0, (u32)-47);
            *resp_len = out_len;
            return 0;
        case 52: /* DISASSOC */
            queue_event(BRCM_E_LINK, 0, 0, 0);
            return 0;
        case 185: case 187: case 258: case 219: /* scan times, fakefrag */
            return 0;
        default:
            return -23;
    }
}

static void dongle_msg(int ring, const u8 *m)
{
    u8 type = m[0];
    if (ring == 2) { /* TX flowring: msgbuf_tx_msghdr */
        CHECK(type == 0x0f && m[23] == 1 && (m[22] & 7) == 1);
        u32 dlen = g16(m, 42);
        static u8 frame[2048];
        memcpy(frame, m + 8, 14);
        memcpy(frame + 14, host(g64(m, 32), dlen), dlen);
        extern void dongle_tx(const u8 *f, u32 len);
        dongle_tx(frame, 14 + dlen);
        u8 st[24] = {0};
        st[0] = 0x10;
        p32(st, 4, g32(m, 4));
        p16(st, 10, 2);
        d2h_put(1, st, 16);
        return;
    }
    switch (type) {
        case 0x11: /* RXBUF_POST */
            CHECK(ring == 1 && g16(m, 10) >= 1536);
            if (n_rx < NBUF)
                rx_free[n_rx++] = (struct dbuf){g32(m, 4), g64(m, 24), g16(m, 10)};
            break;
        case 0x0d: /* EVENT_BUF_POST */
        case 0x0b: /* IOCTLRESP_BUF_POST */
            CHECK(ring == 0 && g16(m, 8) >= 2048);
            if (type == 0x0d && n_ev < NBUF)
                ev_free[n_ev++] = (struct dbuf){g32(m, 4), g64(m, 16), g16(m, 8)};
            if (type == 0x0b && n_io < NBUF)
                io_free[n_io++] = (struct dbuf){g32(m, 4), g64(m, 16), g16(m, 8)};
            break;
        case 0x09: { /* IOCTLPTR_REQ */
            CHECK(ring == 0 && g32(m, 4) == 0xfffe);
            u32 cmd = g32(m, 8);
            u16 trans = g16(m, 12), in_len = g16(m, 14), out_len = g16(m, 16);
            CHECK(in_len <= 1518 && out_len <= 8192);
            static u8 in[2048], resp[8192];
            memcpy(in, host(g64(m, 24), in_len), in_len);
            u32 resp_len = 0;
            s16 status = fw_ioctl(cmd, in, in_len, out_len, resp, &resp_len);
            u8 ack[24] = {0};
            ack[0] = 0x0a;
            p32(ack, 4, 0xfffe);
            d2h_put(0, ack, 24);
            CHECK(n_io > 0);
            if (n_io <= 0)
                break;
            struct dbuf b = io_free[0];
            memmove(io_free, io_free + 1, --n_io * sizeof(*io_free));
            memcpy(host(b.addr, min(resp_len, (u32)b.len)), resp, min(resp_len, (u32)b.len));
            u8 c[24] = {0};
            c[0] = 0x0c;
            p32(c, 4, b.id);
            p16(c, 8, (u16)status);
            p16(c, 12, resp_len);
            p16(c, 14, trans);
            p32(c, 16, cmd);
            d2h_put(0, c, 24);
            break;
        }
        case 0x03: { /* FLOW_RING_CREATE */
            CHECK(ring == 0 && g16(m, 22) == 2 && m[20] == 0);
            CHECK(tcm_read16(H2D_W + 8) == 0 && tcm_read16(H2D_R + 8) == 0);
            h2d[2] = (struct dring){g64(m, 32), g16(m, 28), g16(m, 30), 0};
            CHECK(h2d[2].depth >= 64 && h2d[2].item == 48);
            flow_creates++;
            u8 c[24] = {0};
            c[0] = 0x04;
            p16(c, 10, 2);
            d2h_put(0, c, 24);
            break;
        }
        case 0x23: /* H2D_MAILBOX_DATA */
            CHECK(ring == 0 && m[1] == 0xff);
            if (g32(m, 8) == 2)
                h2d_mb_ds_ack++;
            break;
        default:
            printf("dongle: unexpected message 0x%x on ring %d\n", type, ring);
            failures++;
    }
}

static void deliver_event(const struct pend_event *e)
{
    CHECK(n_ev > 0);
    if (n_ev <= 0)
        return;
    struct dbuf b = ev_free[0];
    memmove(ev_free, ev_free + 1, --n_ev * sizeof(*ev_free));
    u8 pkt[80] = {0};
    memcpy(pkt, fw_mac, 6);
    memcpy(pkt + 6, ap_mac, 6);
    pbe16(pkt, 12, 0x886c);
    pbe16(pkt, 14, 0x8001); /* brcm_ethhdr subtype */
    pbe16(pkt, 16, 48 + 8);
    pkt[19] = 0x00, pkt[20] = 0x10, pkt[21] = 0x18; /* BRCM OUI */
    pbe16(pkt, 22, 1);
    u8 *em = pkt + 24; /* brcmf_event_msg_be */
    pbe16(em, 0, 2);
    pbe16(em, 2, e->flags);
    pbe32(em, 4, e->type);
    pbe32(em, 8, e->status);
    pbe32(em, 12, e->reason);
    pbe32(em, 20, 8);
    memcpy(em + 24, ap_mac, 6);
    memcpy(em + 30, "wlan0", 5);
    memcpy(host(b.addr, sizeof(pkt)), pkt, sizeof(pkt));
    u8 c[24] = {0};
    c[0] = 0x0e;
    p32(c, 4, b.id);
    p16(c, 12, sizeof(pkt));
    d2h_put(0, c, 24);
}

static void load_rings(void)
{
    if (h2d[0].depth)
        return;
    for (int i = 0; i < 5; i++) {
        u32 d = RINGMEM + 16 * i;
        struct dring r = {g64(tcm, d + 8), g16(tcm, d + 4), g16(tcm, d + 6), 0};
        if (i < 2)
            h2d[i] = r;
        else
            d2h[i - 2] = r;
    }
}

static int in_service;
static void dongle_service(void)
{
    if (in_service || !map_va)
        return;
    in_service = 1;
    load_rings();
    for (int ring = 0; ring < 3; ring++) {
        struct dring *r = &h2d[ring];
        if (!r->depth)
            continue;
        u16 w = tcm_read16(H2D_W + 4 * ring);
        CHECK(w < r->depth);
        while (r->idx != w && w < r->depth) {
            dongle_msg(ring, host(r->iova + (u64)r->idx * r->item, r->item));
            r->idx = (r->idx + 1) % r->depth;
        }
        tcm_write16(H2D_R + 4 * ring, r->idx);
    }
    while (evq_n && n_ev) {
        deliver_event(&evq[0]);
        memmove(evq, evq + 1, --evq_n * sizeof(*evq));
    }
    in_service = 0;
}

/* ---------- traffic ---------- */

static u8 tx_seen[2048];
static u32 tx_seen_len, tx_count;
void dongle_tx(const u8 *f, u32 len)
{
    memcpy(tx_seen, f, len);
    tx_seen_len = len;
    tx_count++;
}

static void inject_rx(const u8 *frame, u32 len, u16 offset, u16 flags)
{
    CHECK(n_rx > 0);
    if (n_rx <= 0)
        return;
    struct dbuf b = rx_free[0];
    memmove(rx_free, rx_free + 1, --n_rx * sizeof(*rx_free));
    memcpy(host(b.addr + offset, len), frame, len);
    u8 c[40] = {0};
    c[0] = 0x12;
    p32(c, 4, b.id);
    p16(c, 14, len);
    p16(c, 16, offset);
    p16(c, 18, flags);
    d2h_put(2, c, 40);
}

static void d2h_mailbox(u32 data)
{
    u8 c[24] = {0};
    c[0] = 0x24;
    p32(c, 12, data);
    d2h_put(0, c, 24);
}

static u8 rx_last[2048];
static u32 rx_last_len, rx_count;
static void on_rx(const u8 *f, u32 len)
{
    memcpy(rx_last, f, len);
    rx_last_len = len;
    rx_count++;
}

static int find_call(int from, u32 cmd, const char *name)
{
    for (int i = from; i < ncalls; i++)
        if (calls[i].cmd == cmd && (!name || !strcmp(calls[i].name, name)))
            return i;
    return -1;
}

int main(void)
{
    /* firmware's shared area and ring info (brcmf_pcie_dhi_ringinfo) */
    p32(tcm, RINGINFO + 0, RINGMEM);
    p32(tcm, RINGINFO + 4, H2D_W);
    p32(tcm, RINGINFO + 8, H2D_R);
    p32(tcm, RINGINFO + 12, D2H_W);
    p32(tcm, RINGINFO + 16, D2H_R);
    p16(tcm, RINGINFO + 52, 40); /* max_flowrings */
    p16(tcm, RINGINFO + 54, 42); /* max_submissionrings */
    p16(tcm, RINGINFO + 56, 3);  /* max_completionrings */
    /* stale flowring indices: the driver must zero them before FLOW_RING_CREATE */
    p16(tcm, H2D_W + 8, 0x77);
    p16(tcm, H2D_R + 8, 0x77);
    brcm.tcm_size = TCM_SIZE;
    brcm.shared = SHARED;
    brcm.ring_info = RINGINFO;
    brcm.version = 7;
    brcm.flags = 7 | 0x10000000; /* HOSTRDY_DB1, mailbox data through the control ring */
    brcm.mb_via_ctl = true;
    brcm.max_rxbufpost = 64;
    memcpy(fw_mac, "\x00\x90\x4c\x01\x02\x03", 6);
    for (int i = 0; i < 32; i++)
        good_pmk[i] = 0x40 + i;

    /* ---- rings ---- */
    CHECK(brcm_msgbuf_init() == 0);
    CHECK(hostready == 1 && doorbells > 0 && doorbells_before_hostready == 0);
    CHECK(g16(tcm, RINGMEM + 4) == 64 && g16(tcm, RINGMEM + 6) == 40);      /* control submit */
    CHECK(g16(tcm, RINGMEM + 16 + 6) == 32);                                /* rx post */
    CHECK(g16(tcm, RINGMEM + 32 + 6) == 24 && g16(tcm, RINGMEM + 48 + 6) == 24 &&
          g16(tcm, RINGMEM + 64 + 6) == 40); /* v7 completion item sizes */
    CHECK(g32(tcm, SHARED + 52) == 8 && g32(tcm, SHARED + 64) == 1024); /* scratch, ringupd */
    CHECK(n_rx == 64 && n_ev == 8 && n_io == 4);

    /* ---- dongle init ---- */
    static u8 clm[3000], txcap[100];
    for (u32 i = 0; i < sizeof(clm); i++)
        clm[i] = i * 7 + 3;
    struct brcm_fw_set fw;
    memset(&fw, 0, sizeof(fw));
    fw.clm = (u64)(uintptr_t)clm;
    fw.clm_len = sizeof(clm);
    fw.txcap = (u64)(uintptr_t)txcap;
    fw.txcap_len = sizeof(txcap);
    const u8 adt_mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    memcpy(&fw.mac, adt_mac, 6);
    CHECK(brcm_dongle_init(&fw) == 0);
    CHECK(!memcmp(fw_mac, adt_mac, 6) && !memcmp(brcm_mac, adt_mac, 6));
    CHECK(clm_chunks == 3 && clm_flags[0] == 0x1002 && clm_flags[1] == 0x1000 &&
          clm_flags[2] == 0x1004);
    CHECK(clm_len_got == sizeof(clm) && !memcmp(clm_got, clm, sizeof(clm)));
    CHECK(txcap_chunks == 1 && txcap_flag == 0x1006);
    CHECK(event_mask_len == 27);
    static const u8 wanted[] = {0, 1, 3, 5, 6, 7, 9, 11, 12, 16, 46, 54};
    u8 expect_mask[32] = {0};
    for (u32 i = 0; i < sizeof(wanted); i++)
        expect_mask[wanted[i] / 8] |= 1 << (wanted[i] % 8);
    CHECK(!memcmp(event_mask, expect_mask, 27));
    CHECK(fw_is_up && pm_mode == 2 && infra == 1 && allmulti);
    /* order of brcmf_c_preinit_dcmds, then interface up */
    int a = find_call(0, 263, "cur_etheraddr"), b = find_call(a, 98, NULL),
        c = find_call(b, 263, "clmload"), d = find_call(c, 263, "txcapload"),
        e = find_call(d, 262, "ver"), f = find_call(e, 263, "mpc"),
        g = find_call(f, 263, "join_pref"), h = find_call(g, 263, "event_msgs_ext"),
        i = find_call(h, 2, NULL), j = find_call(i, 86, NULL);
    CHECK(a >= 0 && b > a && c > b && d > c && e > d && f > e && g > f && h > g && i > h && j > i);
    if (g >= 0)
        CHECK(calls[g].len == 8 && !memcmp(calls[g].data, "\x04\x02\x08\x01\x01\x02\x00\x00", 8));

    /* ---- ioctl channel across many wraps of the 64-entry control rings ---- */
    int ok = 0;
    for (int n = 0; n < 300; n++) {
        u32 v = 0;
        ok += brcm_iovar_int_get("mpc", &v) == 0 && v == 1;
    }
    CHECK(ok == 300);
    u32 v = 0;
    CHECK(brcm_iovar_int_get("no_such_iovar", &v) == -23);

    /* ---- WPA2-PSK join: wrong PMK, then the passphrase retry ---- */
    u8 bad_pmk[32];
    memset(bad_pmk, 0x55, sizeof(bad_pmk));
    int first = ncalls;
    CHECK(brcm_join((const u8 *)"TestNet", 7, bad_pmk, good_pass, strlen(good_pass)) == 0);
    CHECK(joins == 2 && pmk_flags == 1 && pmk_len == strlen(good_pass));
    /* cfg80211.c connect order: wpaie, wpa_auth, auth, wsec, mfp, wpa_auth, sup_wpa, PMK, join */
    int s0 = find_call(first, 263, "wpaie"), s1 = find_call(s0, 263, "wpa_auth"),
        s2 = find_call(s1, 263, "auth"), s3 = find_call(s2, 263, "wsec"),
        s4 = find_call(s3, 263, "mfp"), s5 = find_call(s4 + 1, 263, "wpa_auth"),
        s6 = find_call(s5, 263, "sup_wpa"), s7 = find_call(s6, 268, NULL),
        s8 = find_call(s7, 263, "join");
    CHECK(s0 >= 0 && s1 > s0 && s2 > s1 && s3 > s2 && s4 > s3 && s5 > s4 && s6 > s5 &&
          s7 > s6 && s8 > s7);
    if (s1 >= 0 && s5 >= 0 && s7 >= 0) {
        CHECK(g32(calls[s1].data, 0) == 0xc0 && g32(calls[s5].data, 0) == 0x80);
        CHECK(g16(calls[s7].data, 0) == 32 && g16(calls[s7].data, 2) == 0 &&
              !memcmp(calls[s7].data + 4, bad_pmk, 32));
    }
    if (s0 >= 0)
        CHECK(calls[s0].len == 22 && calls[s0].data[0] == 0x30 && calls[s0].data[1] == 20);
    /* brcmf_ext_join_params_v1_le, no channel: 4 + 36 + 20 + 16 */
    CHECK(last_join_len == 76);
    CHECK(g16(last_join, 0) == 1 && g32(last_join, 4) == 7 && !memcmp(last_join + 8, "TestNet", 7));
    CHECK(last_join[40] == 0 && g32(last_join, 44) == 0xffffffff && g32(last_join, 48) == 0xffffffff &&
          g32(last_join, 52) == 0xffffffff && g32(last_join, 56) == 0xffffffff);
    CHECK(g16(last_join, 60) == 1 && g16(last_join, 62) == 0 &&
          !memcmp(last_join + 64, "\xff\xff\xff\xff\xff\xff", 6) && g16(last_join, 70) == 0 &&
          g32(last_join, 72) == 0);
    const struct brcm_link *l = brcm_link_state();
    CHECK(l->link && l->keyed);

    /* the right PMK joins on the first attempt */
    CHECK(brcm_join((const u8 *)"TestNet", 7, good_pmk, good_pass, strlen(good_pass)) == 0);
    CHECK(joins == 3 && pmk_flags == 0);

    /* ---- TX through the flowring ---- */
    u8 frame[1514];
    memcpy(frame, ap_mac, 6);
    memcpy(frame + 6, adt_mac, 6);
    frame[12] = 0x08, frame[13] = 0x00;
    for (u32 n = 14; n < sizeof(frame); n++)
        frame[n] = n ^ 0x5a;
    CHECK(brcm_tx(frame, 60) == -1); /* first frame: flowring create, frame dropped */
    brcm_poll();
    CHECK(flow_creates == 1);
    CHECK(brcm_tx(frame, 60) == 0);
    CHECK(tx_count == 1 && tx_seen_len == 60 && !memcmp(tx_seen, frame, 60));
    ok = 0;
    for (u32 n = 0; n < 700; n++) {
        frame[20] = n;
        u32 len = 60 + (n * 37) % 1454;
        ok += brcm_tx(frame, len) == 0 && tx_seen_len == len && !memcmp(tx_seen, frame, len);
        brcm_poll();
    }
    CHECK(ok == 700 && tx_count == 701 && l->tx_errors == 0);

    /* ---- RX ---- */
    brcm_set_callbacks(on_rx, NULL);
    inject_rx(frame, 98, 0, 1); /* 802.3 */
    brcm_poll();
    CHECK(rx_count == 1 && rx_last_len == 98 && !memcmp(rx_last, frame, 98));
    inject_rx(frame + 1, 70, 6, 0); /* flags 0 and an explicit data offset: still 802.3 */
    brcm_poll();
    CHECK(rx_count == 2 && rx_last_len == 70 && !memcmp(rx_last, frame + 1, 70));
    inject_rx(frame, 64, 0, 2); /* 802.11 (monitor) frame: dropped */
    brcm_poll();
    CHECK(rx_count == 2 && l->rx_dropped == 1);
    ok = 0;
    for (int round = 0; round < 40; round++) { /* 1600 frames: rx post and complete wrap */
        for (int k = 0; k < 40; k++) {
            frame[30] = round, frame[31] = k;
            inject_rx(frame, 200, 0, 1);
        }
        u32 before = rx_count;
        brcm_poll();
        ok += rx_count == before + 40 && rx_last[30] == round && rx_last[31] == 39;
        udelay(10); /* the dongle takes the reposted buffers */
    }
    CHECK(ok == 40 && n_rx == 64);

    /* ---- D2H mailbox data through the control ring: deep sleep request -> ack ---- */
    d2h_mailbox(0x2);
    brcm_poll();
    udelay(10);
    CHECK(h2d_mb_ds_ack == 1);

    /* ---- link loss ---- */
    queue_event(BRCM_E_DEAUTH_IND, 0, 7, 0);
    udelay(10);
    brcm_poll();
    CHECK(!l->link && !l->keyed && l->last_reason == 7);
    CHECK(brcm_tx(frame, 60) == -1);

    CHECK(unaligned_syncs == 0);
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
