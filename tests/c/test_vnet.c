/* SPDX-License-Identifier: MIT */
/*
 * Host test of m1n1's virtio-net device model (m1n1_windows/src/hv_vnet.c), driven the way
 * Red Hat's NetKVM / VirtIOPCIModern.c drive a modern virtio-pci device: PCI capability walk,
 * BAR sizing and programming, reset, feature negotiation with FEATURES_OK read-back, split
 * virtqueue setup with two-part 64-bit address writes, DRIVER_OK, then traffic: RX buffers
 * as [header][data] chains or single buffers, TX chains, notify writes, ISR read-to-clear,
 * INTx injection only on the boot CPU and only once per EOI, NO_INTERRUPT suppression,
 * link status through the device config + config interrupt, ring wrap-around, device
 * errors -> NEEDS_RESET, reset, BAR moves. Layouts come from the virtio 1.2 spec, written
 * out here independently of hv_vnet.c.
 */
#include "vnet_host_test.h"

#include "hv_vnet.h"

static int failures;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                    \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/* ---------- m1n1 environment ---------- */

int boot_cpu_idx = 0;
static int cur_cpu;
int smp_id(void) { return cur_cpu; }
static int ipis;
void smp_send_ipi(int cpu)
{
    CHECK(cpu == boot_cpu_idx);
    ipis++;
}

static u64 ticks;
u64 get_ticks(void) { return ticks; }
u64 ticks_to_msecs(u64 t) { return t / 1000; }

static hv_hook_t *hook;
static u64 hook_base, hook_size;
static int unmaps;
int hv_map_hook(u64 from, hv_hook_t *h, u64 size)
{
    hook = h;
    hook_base = from;
    hook_size = size;
    return 0;
}
int hv_unmap(u64 from, u64 size)
{
    CHECK(from == hook_base && size == hook_size);
    unmaps++;
    hook = NULL;
    return 0;
}

/* guest RAM: one arena; its host addresses are the "IPAs" */
#define ARENA (1u << 22)
static u8 *ram;
u64 hv_ipa_to_pa(u64 ipa)
{
    u64 b = (u64)(uintptr_t)ram;
    return ipa >= b && ipa < b + ARENA ? ipa : 0;
}
static u64 ipa(const void *p) { return (u64)(uintptr_t)p; }

static bool gic_enabled = true;
static int free_lr = 0, injected;
bool hv_vgic3_irq_enabled(u32 v) { return v == 67 && gic_enabled; }
int hv_vgic3_get_free_lr(void) { return free_lr; }
u8 hv_vgic3_get_priority(u32 v) { (void)v; return 0xa0; }
void hv_vgic3_inject_irq(u32 v, u8 prio, bool active, bool pending, bool hw, u64 hw_irq)
{
    CHECK(v == 67 && prio == 0xa0 && !active && pending && !hw && !hw_irq);
    CHECK(cur_cpu == boot_cpu_idx);
    injected++;
}

static u8 tx_last[4096];
static u32 tx_last_len, tx_count;
static int tx_fail;
int brcm_tx(const u8 *f, u32 len)
{
    if (tx_fail)
        return -1;
    memcpy(tx_last, f, len);
    tx_last_len = len;
    tx_count++;
    return 0;
}

/* ---------- guest side helpers ---------- */

static u32 cfg32(u32 reg) { return hv_vnet_cfg_read(reg); }
static u8 cfg8(u32 off) { return cfg32(off & ~3u) >> (8 * (off & 3)); }
static void cfgw(u32 reg, u32 v) { hv_vnet_cfg_write(reg, 0, 4, v); }

static u64 bar;
static u64 mmio_r(u32 off, int width)
{
    u64 v = 0xdeadbeef;
    CHECK(hook && hook(NULL, bar + off, &v, false, width));
    return v;
}
static void mmio_w(u32 off, u64 v, int width)
{
    CHECK(hook && hook(NULL, bar + off, &v, true, width));
}

/* virtio_pci_common_cfg offsets */
#define DFSEL 0x00
#define DF    0x04
#define GFSEL 0x08
#define GF    0x0c
#define MSIXC 0x10
#define NUMQ  0x12
#define STAT  0x14
#define GEN   0x15
#define QSEL  0x16
#define QSIZE 0x18
#define QMSIX 0x1a
#define QEN   0x1c
#define QNOFF 0x1e
#define QDESC 0x20
#define QAVL  0x28
#define QUSED 0x30

struct guest_q {
    u16 num;
    u8 *desc, *avail, *used;
    u16 avail_idx, used_seen;
    u32 notify;
};
static struct guest_q gq[2];
static u32 cap_common, cap_isr, cap_dev, cap_notify, notify_mult;

static void put16(u8 *p, u16 v) { memcpy(p, &v, 2); }
static u16 get16(const u8 *p) { u16 v; memcpy(&v, p, 2); return v; }
static u32 get32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }

static void set_desc(struct guest_q *q, u16 i, const void *addr, u32 len, u16 flags, u16 next)
{
    u8 *d = q->desc + 16 * i;
    u64 a = ipa(addr);
    memcpy(d, &a, 8);
    memcpy(d + 8, &len, 4);
    memcpy(d + 12, &flags, 2);
    memcpy(d + 14, &next, 2);
}

static void offer(struct guest_q *q, u16 head)
{
    put16(q->avail + 4 + 2 * (q->avail_idx % q->num), head);
    q->avail_idx++;
    __atomic_store_n((u16 *)(q->avail + 2), q->avail_idx, __ATOMIC_RELEASE);
}

static int used_pending(struct guest_q *q) { return (u16)(get16(q->used + 2) - q->used_seen); }
static void used_pop(struct guest_q *q, u32 *id, u32 *len)
{
    const u8 *e = q->used + 4 + 8 * (q->used_seen % q->num);
    *id = get32(e);
    *len = get32(e + 4);
    q->used_seen++;
}

/* NetKVM's ISR: read (and so ack) the ISR byte; then the vGIC EOI */
static u8 guest_isr_and_eoi(void)
{
    u8 isr = mmio_r(cap_isr, 0);
    hv_vnet_irq_eoi(67);
    hv_vnet_poll_irq(); /* hv_exc_exit after the EOI trap */
    return isr;
}

static u8 *alloc_page(void)
{
    static u32 used = 0;
    u8 *p = ram + used;
    used += 0x1000;
    CHECK(used <= ARENA);
    return p;
}

static const u8 mac[6] = {0x3c, 0xa6, 0xf6, 0x11, 0x22, 0x33};

static void setup_queue(int i, u16 num)
{
    struct guest_q *q = &gq[i];
    mmio_w(cap_common + QSEL, i, 1);
    CHECK(mmio_r(cap_common + QSIZE, 1) == 256);
    q->num = num;
    q->notify = mmio_r(cap_common + QNOFF, 1);
    q->desc = alloc_page();
    q->avail = alloc_page();
    q->used = alloc_page() + 64;
    mmio_w(cap_common + QSIZE, num, 1);
    /* iowrite64_twopart */
    mmio_w(cap_common + QDESC, (u32)ipa(q->desc), 2);
    mmio_w(cap_common + QDESC + 4, ipa(q->desc) >> 32, 2);
    mmio_w(cap_common + QAVL, (u32)ipa(q->avail), 2);
    mmio_w(cap_common + QAVL + 4, ipa(q->avail) >> 32, 2);
    mmio_w(cap_common + QUSED, (u32)ipa(q->used), 2);
    mmio_w(cap_common + QUSED + 4, ipa(q->used) >> 32, 2);
    mmio_w(cap_common + QEN, 1, 1);
    CHECK(mmio_r(cap_common + QDESC, 2) == (u32)ipa(q->desc));
    CHECK(mmio_r(cap_common + QUSED + 4, 2) == ipa(q->used) >> 32);
    CHECK(mmio_r(cap_common + QEN, 1) == 1);
    /* fields are frozen once enabled */
    mmio_w(cap_common + QSIZE, 2, 1);
    CHECK(mmio_r(cap_common + QSIZE, 1) == num);
}

static void driver_init(void)
{
    mmio_w(cap_common + STAT, 0, 0); /* reset */
    CHECK(mmio_r(cap_common + STAT, 0) == 0);
    mmio_w(cap_common + STAT, 1, 0);
    mmio_w(cap_common + STAT, 3, 0);
    mmio_w(cap_common + DFSEL, 0, 2);
    u64 f = mmio_r(cap_common + DF, 2);
    mmio_w(cap_common + DFSEL, 1, 2);
    f |= mmio_r(cap_common + DF, 2) << 32;
    CHECK(f == ((1ULL << 32) | (1u << 16) | (1u << 5)));
    mmio_w(cap_common + GFSEL, 0, 2);
    mmio_w(cap_common + GF, (u32)f, 2);
    mmio_w(cap_common + GFSEL, 1, 2);
    mmio_w(cap_common + GF, f >> 32, 2);
    mmio_w(cap_common + STAT, 3 | 8, 0);
    CHECK(mmio_r(cap_common + STAT, 0) == (3 | 8));
    CHECK(mmio_r(cap_common + NUMQ, 1) == 2);
    CHECK(mmio_r(cap_common + MSIXC, 1) == 0xffff);
    setup_queue(0, 256);
    setup_queue(1, 128);
    mmio_w(cap_common + STAT, 3 | 8 | 4, 0);
}

/* guest buffers live in guest RAM (the arena); the device refuses anything else */
static u8 (*rxbufs)[1536];
static u8 (*rxhdrs)[12];
/* rx buffers as NetKVM without mergeable buffers posts them: [header][data] chains (even
   heads) or one buffer with header + data (odd heads) */
static void post_rx(u16 slot)
{
    struct guest_q *q = &gq[0];
    u16 d = (slot * 2) % q->num;
    if (slot & 1) {
        set_desc(q, d, rxbufs[slot % 256], 12 + 1514, 2, 0);
    } else {
        set_desc(q, d, rxhdrs[slot % 256], 12, 2 | 1, d + 1);
        set_desc(q, d + 1, rxbufs[slot % 256], 1514, 2, 0);
    }
    offer(q, d);
}

int main(void)
{
    ram = aligned_alloc(0x1000, ARENA);
    memset(ram, 0, ARENA);
    rxbufs = (void *)(ram + ARENA / 2);
    rxhdrs = (void *)(ram + ARENA / 2 + 256 * 1536);
    u8 *txarea = ram + ARENA / 2 + 256 * 1536 + 256 * 12 + 64;
    u8 *txhdr = txarea, *txdata = txarea + 64, *big = txarea + 64 + 2048;

    /* ---- not present until the Wi-Fi is up ---- */
    CHECK(!hv_vnet_present());
    hv_vnet_rx((const u8 *)"x", 1); /* ignored */
    hv_vnet_init(mac);
    CHECK(hv_vnet_present());

    /* ---- PCI header ---- */
    CHECK(cfg32(0x00) == 0x10411af4);
    CHECK((cfg32(0x08) >> 8) == 0x020000 && (cfg32(0x08) & 0xff) >= 1);
    CHECK(cfg32(0x2c) == 0x11001af4);
    CHECK(cfg32(0x04) & (1u << 20)); /* capabilities list */
    CHECK(((cfg32(0x3c) >> 8) & 0xff) == 1);   /* INTA */
    CHECK(cfg32(0x0c) == 0);                   /* type 0, single function */
    /* capability walk, as find_pci_vendor_capabilities */
    int ncaps = 0;
    for (u8 p = cfg8(0x34); p; p = cfg8(p + 1)) {
        CHECK(cfg8(p) == 0x09 && ++ncaps <= 8);
        u8 type = cfg8(p + 3), barno = cfg8(p + 4);
        u32 off = cfg32(p + 8), len = cfg32(p + 12);
        CHECK(barno == 0 && off + len <= 0x4000);
        if (type == 1) {
            cap_common = off;
            CHECK(len >= 56);
        } else if (type == 3) {
            cap_isr = off;
        } else if (type == 4) {
            cap_dev = off;
            CHECK(len >= 10);
        } else if (type == 2) {
            cap_notify = off;
            notify_mult = cfg32(p + 16);
            CHECK(cfg8(p + 2) == 20 && len >= notify_mult + 2);
        }
    }
    CHECK(ncaps == 4 && cap_isr && cap_dev && cap_notify && notify_mult);

    /* ---- BAR sizing, then programming (UEFI/pci.sys) ---- */
    cfgw(0x10, 0xffffffff);
    cfgw(0x14, 0xffffffff);
    CHECK(!hook);
    u64 size = ~(((u64)cfg32(0x14) << 32) | (cfg32(0x10) & ~0xfu)) + 1;
    CHECK(size == 0x4000 && (cfg32(0x10) & 0x6) == 0x4);
    bar = 0x401000000ULL;
    cfgw(0x10, (u32)bar);
    cfgw(0x14, bar >> 32);
    CHECK(!hook); /* memory decode still off */
    hv_vnet_cfg_write(0x04, 0, 2, 0x6); /* MSE + bus master */
    CHECK(hook && hook_base == bar && hook_size == 0x4000);
    hv_vnet_cfg_write(0x3c, 0, 1, 0x43);
    CHECK((cfg32(0x3c) & 0xff) == 0x43);

    /* ---- device config before the link ---- */
    u8 devmac[6];
    for (int i = 0; i < 6; i++)
        devmac[i] = mmio_r(cap_dev + i, 0);
    CHECK(!memcmp(devmac, mac, 6));
    CHECK(mmio_r(cap_dev + 6, 1) == 0); /* link down */

    /* ---- negotiation refuses a feature we never offered ---- */
    mmio_w(cap_common + STAT, 0, 0);
    mmio_w(cap_common + STAT, 3, 0);
    mmio_w(cap_common + GFSEL, 0, 2);
    mmio_w(cap_common + GF, 1u << 15, 2); /* MRG_RXBUF */
    mmio_w(cap_common + GFSEL, 1, 2);
    mmio_w(cap_common + GF, 1, 2);
    mmio_w(cap_common + STAT, 3 | 8, 0);
    CHECK(!(mmio_r(cap_common + STAT, 0) & 8));
    /* ... and VERSION_1 is required */
    mmio_w(cap_common + STAT, 0, 0);
    mmio_w(cap_common + STAT, 3, 0);
    mmio_w(cap_common + GFSEL, 0, 2);
    mmio_w(cap_common + GF, 1u << 5, 2);
    mmio_w(cap_common + STAT, 3 | 8, 0);
    CHECK(!(mmio_r(cap_common + STAT, 0) & 8));

    driver_init();
    CHECK(gq[0].notify == 0 && gq[1].notify == 1);

    /* ---- link up: config interrupt, injected once until EOI ---- */
    u8 gen0 = mmio_r(cap_common + GEN, 0);
    hv_vnet_set_link(true);
    CHECK(injected == 1);
    CHECK(mmio_r(cap_dev + 6, 1) == 1 && (u8)mmio_r(cap_common + GEN, 0) != gen0);
    hv_vnet_poll_irq();
    CHECK(injected == 1); /* outstanding */
    CHECK(guest_isr_and_eoi() == 2);
    CHECK(injected == 1); /* ISR clear: nothing to re-inject */

    /* ---- RX ---- */
    for (u16 s = 0; s < 64; s++)
        post_rx(s);
    u8 frame[1514];
    for (u32 i = 0; i < sizeof(frame); i++)
        frame[i] = i * 13 + 7;
    memcpy(frame, mac, 6);
    hv_vnet_rx(frame, 98);
    CHECK(injected == 2 && used_pending(&gq[0]) == 1);
    u32 id, len;
    used_pop(&gq[0], &id, &len);
    CHECK(id == 0 && len == 12 + 98);
    CHECK(rxhdrs[0][0] == 0 && rxhdrs[0][1] == 0 && get16(rxhdrs[0] + 10) == 1); /* num_buffers */
    CHECK(!memcmp(rxbufs[0], frame, 98));
    /* a second frame while the first interrupt is outstanding: no second injection ... */
    hv_vnet_rx(frame, 1514);
    CHECK(injected == 2);
    /* ... until the EOI; the guest ISR read then sees the queue bit */
    CHECK(guest_isr_and_eoi() == 1);
    hv_vnet_irq_eoi(67);
    hv_vnet_poll_irq();
    used_pop(&gq[0], &id, &len);
    CHECK(id == 2 && len == 12 + 1514 && get16(rxbufs[1] + 10) == 1 && !memcmp(rxbufs[1] + 12, frame, 1514));
    /* NO_INTERRUPT: used entries without an interrupt */
    put16(gq[0].avail, 1);
    int inj = injected;
    hv_vnet_rx(frame, 60);
    CHECK(injected == inj && used_pending(&gq[0]) == 1);
    used_pop(&gq[0], &id, &len);
    put16(gq[0].avail, 0);
    /* wrap-around: 1000 frames through 64 buffers that the guest recycles */
    int ok = 0;
    u16 next_slot = 64;
    for (int n = 0; n < 1000; n++) {
        frame[20] = n, frame[21] = n >> 8;
        hv_vnet_rx(frame, 60 + n % 1400);
        if (used_pending(&gq[0]) != 1)
            continue;
        used_pop(&gq[0], &id, &len);
        u16 slot = (id / 2) % 256; /* posted slot for that head */
        (void)slot;
        ok += len == 12u + 60 + n % 1400;
        guest_isr_and_eoi();
        post_rx(next_slot++);
    }
    CHECK(ok == 1000);
    /* out of buffers: dropped, nothing used */
    for (int n = 0; n < 200; n++)
        hv_vnet_rx(frame, 60);
    /* 64 posted, 3 used before the loop and never replaced: 61 left, then drops */
    CHECK(used_pending(&gq[0]) == 61);
    while (used_pending(&gq[0]))
        used_pop(&gq[0], &id, &len);
    guest_isr_and_eoi();

    /* ---- TX ---- */
    memcpy(txdata, frame, 1514);
    struct guest_q *t = &gq[1];
    set_desc(t, 0, txhdr, 12, 1, 1);
    set_desc(t, 1, txdata, 342, 0, 0);
    offer(t, 0);
    mmio_w(cap_notify + notify_mult * t->notify, 1, 1);
    CHECK(tx_count == 1 && tx_last_len == 342 && !memcmp(tx_last, txdata, 342));
    CHECK(used_pending(t) == 1);
    used_pop(t, &id, &len);
    CHECK(id == 0 && len == 0);
    CHECK(guest_isr_and_eoi() == 1);
    /* header and data in one buffer (ANY_LAYOUT) */
    memcpy(big, txhdr, 12);
    memcpy(big + 12, txdata, 1514);
    set_desc(t, 2, big, 12 + 1514, 0, 0);
    offer(t, 2);
    mmio_w(cap_notify + notify_mult * t->notify, 1, 1);
    CHECK(tx_count == 2 && tx_last_len == 1514 && !memcmp(tx_last, txdata, 1514));
    used_pop(t, &id, &len);
    guest_isr_and_eoi();
    /* oversized: completed, not sent */
    set_desc(t, 3, big, 2100, 0, 0);
    offer(t, 3);
    mmio_w(cap_notify + notify_mult * t->notify, 1, 1);
    CHECK(tx_count == 2 && used_pending(t) == 1);
    used_pop(t, &id, &len);
    guest_isr_and_eoi();
    /* notify from another vCPU: the boot CPU gets an IPI, injection waits for it */
    cur_cpu = 3;
    inj = injected;
    set_desc(t, 4, big, 12 + 60, 0, 0);
    offer(t, 4);
    mmio_w(cap_notify + notify_mult * t->notify, 1, 1);
    CHECK(tx_count == 3 && ipis == 1 && injected == inj);
    hv_vnet_poll_irq(); /* still CPU 3: nothing */
    CHECK(injected == inj);
    cur_cpu = 0;
    hv_vnet_poll_irq(); /* the IPI's exception exit on the boot CPU */
    CHECK(injected == inj + 1);
    used_pop(t, &id, &len);
    guest_isr_and_eoi();
    /* 600 frames through the 128-entry TX ring */
    ok = 0;
    for (int n = 0; n < 600; n++) {
        u16 d = 8 + (n % 60) * 2;
        txdata[30] = n;
        set_desc(t, d, txhdr, 12, 1, d + 1);
        set_desc(t, d + 1, txdata, 60 + n, 0, 0);
        offer(t, d);
        mmio_w(cap_notify + notify_mult * t->notify, 1, 1);
        ok += tx_last_len == 60u + n && tx_last[30] == (u8)n && used_pending(t) == 1;
        used_pop(t, &id, &len);
        guest_isr_and_eoi();
    }
    CHECK(ok == 600);
    /* link down: frames completed but not sent */
    hv_vnet_set_link(false);
    CHECK(guest_isr_and_eoi() == 2 && mmio_r(cap_dev + 6, 1) == 0);
    u32 before = tx_count;
    offer(t, 4);
    mmio_w(cap_notify + notify_mult * t->notify, 1, 1);
    CHECK(tx_count == before && used_pending(t) == 1);
    used_pop(t, &id, &len);
    guest_isr_and_eoi();
    hv_vnet_set_link(true);
    guest_isr_and_eoi();

    /* ---- the GIC line not enabled yet: no injection until it is ---- */
    post_rx(next_slot++);
    gic_enabled = false;
    inj = injected;
    hv_vnet_rx(frame, 60);
    CHECK(injected == inj);
    gic_enabled = true;
    hv_vnet_poll_irq();
    CHECK(injected == inj + 1);
    guest_isr_and_eoi();
    while (used_pending(&gq[0]))
        used_pop(&gq[0], &id, &len);

    /* ---- device error: a TX descriptor marked device-writable -> NEEDS_RESET ---- */
    set_desc(t, 6, big, 100, 2, 0);
    offer(t, 6);
    mmio_w(cap_notify + notify_mult * t->notify, 1, 1);
    CHECK(mmio_r(cap_common + STAT, 0) & 0x40);
    CHECK(guest_isr_and_eoi() & 2);
    before = tx_count;
    hv_vnet_rx(frame, 60); /* stopped until reset */
    CHECK(used_pending(&gq[0]) == 0);

    /* ---- reset and bring-up again (NetKVM's NEEDS_RESET handling) ---- */
    memset(gq, 0, sizeof(gq));
    driver_init();
    hv_vnet_set_link(true); /* unchanged: no event */
    for (u16 s = 0; s < 8; s++)
        post_rx(s);
    hv_vnet_rx(frame, 77);
    CHECK(used_pending(&gq[0]) == 1);
    used_pop(&gq[0], &id, &len);
    CHECK(len == 12 + 77);
    guest_isr_and_eoi();

    /* ---- BAR moved by pci.sys ---- */
    bar = 0x402000000ULL;
    cfgw(0x10, (u32)bar);
    cfgw(0x14, bar >> 32);
    CHECK(unmaps == 1 && hook && hook_base == bar);
    CHECK(mmio_r(cap_common + NUMQ, 1) == 2);
    /* memory decode off: trap removed */
    hv_vnet_cfg_write(0x04, 0, 2, 0x0);
    CHECK(unmaps == 2 && !hook);

    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
