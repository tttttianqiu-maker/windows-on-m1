/* SPDX-License-Identifier: MIT */
/*
 * Apple MTP DockChannel HID protocol core. See apple_dchid.h for the wire format.
 * Flow and constants follow Asahi Linux drivers/hid/dockchannel-hid (GPL-2.0 OR MIT).
 */
#include "apple_dchid.h"

#ifndef _KERNEL_MODE
#include <string.h>
#else
#define memcpy RtlCopyMemory
#define memset(d, v, n) RtlFillMemory((d), (n), (UCHAR)(v))
#define memmove RtlMoveMemory
#endif

#define GPIO_UNSUPPORTED_RETCODE 0xe000f00du /* what Linux answers for unknown GPIO cmds */
#define INIT_HDR_SIZE  22                    /* type unk unk iface name[16] more pad */
#define INIT_BLK_SIZE  4                     /* type:le16 length:le16 */
#define GPIO_REQ_SIZE  (4 + DCHID_GPIO_NAME_SIZE)
#define GPIO_CMD_SIZE  5                     /* type iface gpio unk cmd */
#define FW_HDR_SIZE    20

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static size_t round_up4(size_t n) { return (n + 3u) & ~(size_t)3u; }

static bool name_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* ---------------------------------------------------------------- framing */

uint32_t dchid_checksum(const uint8_t *p, size_t len)
{
    uint32_t sum = 0;
    for (; len >= 4; p += 4, len -= 4)
        sum += rd32(p);
    return sum;
}

enum dchid_status dchid_encode(uint8_t iface, uint8_t seq, uint8_t flags,
                               const uint8_t *msg, size_t msg_len,
                               uint8_t *out, size_t cap, size_t *out_len)
{
    size_t padded = round_up4(msg_len);
    size_t body = DCHID_SUBHDR_SIZE + padded;
    size_t total = DCHID_HDR_SIZE + body + DCHID_CHECKSUM_SIZE;
    uint32_t sum;

    if ((!msg && msg_len) || !out || !out_len || msg_len > 0xffff || body > 0xffff)
        return DCHID_E_ARG;
    if (cap < total)
        return DCHID_E_SPACE;

    memset(out, 0, total);
    out[0] = DCHID_HDR_SIZE;
    out[1] = DCHID_CHANNEL_CMD;
    wr16(out + 2, (uint16_t)body);
    out[4] = seq;
    out[5] = iface;
    /* sub header */
    out[8] = flags;
    wr16(out + 10, (uint16_t)msg_len);
    if (msg_len)
        memcpy(out + DCHID_HDR_SIZE + DCHID_SUBHDR_SIZE, msg, msg_len);

    sum = dchid_checksum(out, DCHID_HDR_SIZE + body);
    wr32(out + DCHID_HDR_SIZE + body, 0xffffffffu - sum);
    *out_len = total;
    return DCHID_OK;
}

enum dchid_status dchid_decode(const uint8_t *pkt, size_t len, struct dchid_packet *out)
{
    uint16_t body;
    uint16_t sub_len;

    if (!pkt || !out)
        return DCHID_E_ARG;
    if (len < DCHID_HDR_SIZE || pkt[0] != DCHID_HDR_SIZE)
        return DCHID_E_HEADER;
    body = rd16(pkt + 2);
    if ((body & 3u) || body > DCHID_MAX_BODY || len != (size_t)DCHID_HDR_SIZE + body + DCHID_CHECKSUM_SIZE)
        return DCHID_E_HEADER;
    if (dchid_checksum(pkt, len) != 0xffffffffu)
        return DCHID_E_CHECKSUM;
    if (body < DCHID_SUBHDR_SIZE)
        return DCHID_E_SUBHDR;
    sub_len = rd16(pkt + DCHID_HDR_SIZE + 2);
    if ((size_t)sub_len + DCHID_SUBHDR_SIZE > body)
        return DCHID_E_SUBHDR;

    out->channel = pkt[1];
    out->seq = pkt[4];
    out->iface = pkt[5];
    out->flags = pkt[DCHID_HDR_SIZE];
    out->retcode = rd32(pkt + DCHID_HDR_SIZE + 4);
    out->payload = pkt + DCHID_HDR_SIZE + DCHID_SUBHDR_SIZE;
    out->payload_len = sub_len;
    return DCHID_OK;
}

/* ------------------------------------------------------------- RX stream */

void dchid_rx_reset(struct dchid_rx_stream *s)
{
    s->fill = 0;
    s->dropped = 0;
}

static size_t rx_total(const struct dchid_rx_stream *s)
{
    return (size_t)DCHID_HDR_SIZE + rd16(s->buf + 2) + DCHID_CHECKSUM_SIZE;
}

static size_t rx_needed(const struct dchid_rx_stream *s)
{
    size_t total;
    if (s->fill < DCHID_HDR_SIZE)
        return DCHID_HDR_SIZE - s->fill;
    total = rx_total(s);
    return s->fill >= total ? 0 : total - s->fill;
}

static bool rx_header_plausible(const struct dchid_rx_stream *s)
{
    uint16_t body = rd16(s->buf + 2);
    return s->buf[0] == DCHID_HDR_SIZE && !(body & 3u) && body >= DCHID_SUBHDR_SIZE &&
           body <= DCHID_MAX_BODY;
}

static void rx_drop_one(struct dchid_rx_stream *s)
{
    memmove(s->buf, s->buf + 1, s->fill - 1);
    s->fill--;
    s->dropped++;
}

size_t dchid_rx_push(struct dchid_rx_stream *s, const uint8_t *data, size_t len)
{
    size_t used = 0;

    while (used < len) {
        size_t need, take;

        if (s->fill >= DCHID_HDR_SIZE && rx_needed(s) == 0)
            break; /* a whole packet is waiting to be popped */
        need = rx_needed(s);
        take = need < len - used ? need : len - used;
        memcpy(s->buf + s->fill, data + used, take);
        s->fill += take;
        used += take;

        /* Resynchronise on a corrupt header one byte at a time. */
        while (s->fill >= DCHID_HDR_SIZE && !rx_header_plausible(s))
            rx_drop_one(s);
    }
    return used;
}

enum dchid_status dchid_rx_peek(struct dchid_rx_stream *s, struct dchid_packet *out)
{
    enum dchid_status st;

    if (s->fill < DCHID_HDR_SIZE || rx_needed(s) != 0)
        return DCHID_NEED_MORE;
    st = dchid_decode(s->buf, rx_total(s), out);
    if (st != DCHID_OK) {
        /* A plausible header with a bad checksum: skip a byte and resync. */
        rx_drop_one(s);
        while (s->fill >= DCHID_HDR_SIZE && !rx_header_plausible(s))
            rx_drop_one(s);
    }
    return st;
}

void dchid_rx_pop(struct dchid_rx_stream *s)
{
    size_t total;
    if (s->fill < DCHID_HDR_SIZE || rx_needed(s) != 0)
        return;
    total = rx_total(s);
    memmove(s->buf, s->buf + total, s->fill - total);
    s->fill -= total;
    while (s->fill >= DCHID_HDR_SIZE && !rx_header_plausible(s))
        rx_drop_one(s);
}

/* ------------------------------------------------------- command payloads */

size_t dchid_payload_enable(uint8_t iface, uint8_t out[2])
{
    out[0] = DCHID_CMD_ENABLE_INTERFACE;
    out[1] = iface;
    return 2;
}

size_t dchid_payload_reset(uint8_t iface, uint8_t state, uint8_t out[4])
{
    out[0] = DCHID_CMD_RESET_INTERFACE;
    out[1] = 1;
    out[2] = iface;
    out[3] = state;
    return 4;
}

size_t dchid_payload_firmware(uint8_t iface, uint64_t dma_addr, uint32_t size, uint8_t out[16])
{
    out[0] = DCHID_CMD_SEND_FIRMWARE;
    out[1] = 2;
    out[2] = 0;
    out[3] = iface;
    wr32(out + 4, (uint32_t)dma_addr);
    wr32(out + 8, (uint32_t)(dma_addr >> 32));
    wr32(out + 12, size);
    return 16;
}

size_t dchid_payload_gpio_ack(uint32_t retcode, const uint8_t *cmd, size_t cmd_len,
                              uint8_t *out, size_t cap)
{
    if (cap < 5 + cmd_len)
        return 0;
    out[0] = DCHID_CMD_ACK_GPIO_CMD;
    wr32(out + 1, retcode);
    if (cmd_len)
        memcpy(out + 5, cmd, cmd_len);
    return 5 + cmd_len;
}

/* ---------------------------------------------------------------- session */

static void iface_clear(struct dchid_iface *i)
{
    memset(i, 0, sizeof(*i));
    i->out_report = -1;
}

void dchid_session_init(struct dchid_session *s)
{
    size_t i;
    for (i = 0; i < DCHID_MAX_INTERFACES; i++)
        iface_clear(&s->ifaces[i]);
    s->stm_ready = false;
    /* Interface 0 always exists: it is the event/command channel, not a HID device. */
    s->ifaces[DCHID_IFACE_COMM].present = true;
    memcpy(s->ifaces[DCHID_IFACE_COMM].name, "comm", 5);
}

const struct dchid_iface *dchid_session_find(const struct dchid_session *s, const char *name)
{
    size_t i;
    for (i = 0; i < DCHID_MAX_INTERFACES; i++)
        if (s->ifaces[i].present && name_eq(s->ifaces[i].name, name))
            return &s->ifaces[i];
    return NULL;
}

enum dchid_status dchid_session_command(struct dchid_session *s, uint8_t iface,
                                        uint8_t type, uint8_t req,
                                        const uint8_t *msg, size_t msg_len,
                                        uint8_t *out, size_t cap, size_t *out_len)
{
    struct dchid_iface *i;
    uint8_t flags = DCHID_FLAGS(type, req);
    enum dchid_status st;

    if (iface >= DCHID_MAX_INTERFACES || !msg || !msg_len)
        return DCHID_E_ARG;
    i = &s->ifaces[iface];
    if (!i->present)
        return DCHID_E_IFACE;
    if (i->out_report != -1)
        return DCHID_E_BUSY;
    st = dchid_encode(iface, i->tx_seq, flags, msg, msg_len, out, cap, out_len);
    if (st != DCHID_OK)
        return st;
    /* The ack echoes the first byte (report number / command) of what we sent. */
    i->out_report = msg[0];
    i->out_flags = flags;
    return DCHID_OK;
}

void dchid_session_cancel(struct dchid_session *s, uint8_t iface)
{
    if (iface >= DCHID_MAX_INTERFACES)
        return;
    s->ifaces[iface].out_report = -1;
    s->ifaces[iface].out_flags = 0;
    s->ifaces[iface].tx_seq++;
}

static bool emit(struct dchid_event *ev, size_t max, size_t *n, const struct dchid_event *e)
{
    if (*n >= max)
        return false;
    ev[(*n)++] = *e;
    return true;
}

static enum dchid_status handle_ack(struct dchid_session *s, const struct dchid_packet *p,
                                    struct dchid_event *ev, size_t max, size_t *n)
{
    struct dchid_iface *i = &s->ifaces[p->iface];
    struct dchid_event e;

    if (i->out_report == -1 || p->flags != i->out_flags || p->payload_len < 1 ||
        p->seq != i->tx_seq || p->payload[0] != (uint8_t)i->out_report)
        return DCHID_E_UNEXPECTED;

    memset(&e, 0, sizeof(e));
    e.kind = DCHID_EV_CMD_DONE;
    e.iface = p->iface;
    e.retcode = p->retcode;
    e.data = p->payload + 1;
    e.len = (uint16_t)(p->payload_len - 1);
    i->out_report = -1;
    i->out_flags = 0;
    i->tx_seq++;
    return emit(ev, max, n, &e) ? DCHID_OK : DCHID_E_SPACE;
}

static enum dchid_status emit_enable(struct dchid_session *s, uint8_t idx,
                                     struct dchid_event *ev, size_t max, size_t *n)
{
    struct dchid_event e;
    memset(&e, 0, sizeof(e));
    e.kind = DCHID_EV_ENABLE_IFACE;
    e.iface = idx;
    s->ifaces[idx].deferred = false;
    s->ifaces[idx].enable_sent = true;
    return emit(ev, max, n, &e) ? DCHID_OK : DCHID_E_SPACE;
}

static enum dchid_status handle_init(struct dchid_session *s, const uint8_t *d, size_t len,
                                     struct dchid_event *ev, size_t max, size_t *n)
{
    struct dchid_iface *i;
    uint8_t idx;
    bool more;

    if (len < INIT_HDR_SIZE)
        return DCHID_E_SUBHDR;
    idx = d[3];
    if (idx >= DCHID_MAX_INTERFACES || idx == DCHID_IFACE_COMM)
        return DCHID_E_IFACE;
    i = &s->ifaces[idx];
    if (!i->present) {
        iface_clear(i);
        i->present = true;
        memcpy(i->name, d + 4, DCHID_NAME_SIZE);
        i->name[DCHID_NAME_SIZE] = 0;
    }
    more = d[20] != 0;
    d += INIT_HDR_SIZE;
    len -= INIT_HDR_SIZE;

    while (len >= INIT_BLK_SIZE) {
        uint16_t type = rd16(d), blen = rd16(d + 2);
        d += INIT_BLK_SIZE;
        len -= INIT_BLK_SIZE;
        if (blen > len)
            break;
        switch (type) {
        case DCHID_INIT_HID_DESCRIPTOR:
            if (!i->hid_desc_len && blen <= DCHID_MAX_DESCRIPTOR) {
                memcpy(i->hid_desc, d, blen);
                i->hid_desc_len = blen;
            }
            break;
        case DCHID_INIT_GPIO_REQUEST:
            if (blen >= GPIO_REQ_SIZE && !i->gpio_id) {
                i->gpio_id = rd16(d + 2);
                memcpy(i->gpio_name, d + 4, DCHID_GPIO_NAME_SIZE);
                i->gpio_name[DCHID_GPIO_NAME_SIZE] = 0;
            }
            break;
        default: /* terminator, product name, unknown: nothing to keep */
            break;
        }
        d += blen;
        len -= blen;
        if (type == DCHID_INIT_TERMINATOR)
            break;
    }

    if (more)
        return DCHID_OK;
    i->init_complete = true;
    /* STM goes first: it provides vendor/product IDs for the other interfaces. */
    if (s->stm_ready || name_eq(i->name, "stm"))
        return emit_enable(s, idx, ev, max, n);
    i->deferred = true;
    return DCHID_OK;
}

static enum dchid_status handle_ready(struct dchid_session *s, const uint8_t *d, size_t len,
                                      struct dchid_event *ev, size_t max, size_t *n)
{
    struct dchid_iface *i;
    struct dchid_event e;
    uint8_t idx, k;

    if (len < 2)
        return DCHID_E_SUBHDR;
    idx = d[1];
    if (idx >= DCHID_MAX_INTERFACES || !s->ifaces[idx].present)
        return DCHID_E_IFACE;
    i = &s->ifaces[idx];
    i->ready = true;

    memset(&e, 0, sizeof(e));
    e.kind = DCHID_EV_IFACE_READY;
    e.iface = idx;
    if (!emit(ev, max, n, &e))
        return DCHID_E_SPACE;

    if (name_eq(i->name, "stm") && !s->stm_ready) {
        s->stm_ready = true;
        e.kind = DCHID_EV_STM_READY;
        if (!emit(ev, max, n, &e))
            return DCHID_E_SPACE;
        for (k = 0; k < DCHID_MAX_INTERFACES; k++) {
            if (s->ifaces[k].present && s->ifaces[k].deferred) {
                enum dchid_status st = emit_enable(s, k, ev, max, n);
                if (st != DCHID_OK)
                    return st;
            }
        }
    }
    return DCHID_OK;
}

static enum dchid_status handle_gpio(struct dchid_session *s, const uint8_t *d, size_t len,
                                     struct dchid_event *ev, size_t max, size_t *n)
{
    struct dchid_event e;
    uint8_t idx;

    if (len < GPIO_CMD_SIZE)
        return DCHID_E_SUBHDR;
    memset(&e, 0, sizeof(e));
    e.kind = DCHID_EV_GPIO_PULSE;
    idx = d[1];
    e.iface = idx;
    e.gpio_cmd_len = (uint8_t)(len < sizeof(e.gpio_cmd) ? len : sizeof(e.gpio_cmd));
    memcpy(e.gpio_cmd, d, e.gpio_cmd_len);

    /* Always answer (Linux acks even bad commands, with an error code). */
    if (idx >= DCHID_MAX_INTERFACES || !s->ifaces[idx].present ||
        !s->ifaces[idx].gpio_id || d[2] != s->ifaces[idx].gpio_id || d[4] != 3) {
        e.retcode = GPIO_UNSUPPORTED_RETCODE;
    } else {
        e.gpio_id = s->ifaces[idx].gpio_id;
        e.retcode = 0;
    }
    return emit(ev, max, n, &e) ? DCHID_OK : DCHID_E_SPACE;
}

enum dchid_status dchid_session_handle(struct dchid_session *s, const struct dchid_packet *p,
                                       struct dchid_event *ev, size_t max, size_t *n_ev)
{
    struct dchid_event e;

    if (!s || !p || !ev || !n_ev)
        return DCHID_E_ARG;
    *n_ev = 0;
    if (p->iface >= DCHID_MAX_INTERFACES || !s->ifaces[p->iface].present)
        return DCHID_E_IFACE;

    if (p->channel == DCHID_CHANNEL_CMD)
        return handle_ack(s, p, ev, max, n_ev);

    /* REPORT channel (Linux also treats unknown channels as reports). */
    if ((p->flags >> 6) != DCHID_TYPE_INPUT || !p->payload_len)
        return DCHID_E_SUBHDR;

    if (p->iface == DCHID_IFACE_COMM) {
        switch (p->payload[0]) {
        case DCHID_EVENT_INIT:
            return handle_init(s, p->payload, p->payload_len, ev, max, n_ev);
        case DCHID_EVENT_READY:
            return handle_ready(s, p->payload, p->payload_len, ev, max, n_ev);
        case DCHID_EVENT_GPIO_CMD:
            return handle_gpio(s, p->payload, p->payload_len, ev, max, n_ev);
        default:
            return DCHID_OK; /* unknown comm event: ignore, like Linux */
        }
    }

    memset(&e, 0, sizeof(e));
    e.kind = DCHID_EV_INPUT_REPORT;
    e.iface = p->iface;
    e.data = p->payload;
    e.len = p->payload_len;
    return emit(ev, max, n_ev, &e) ? DCHID_OK : DCHID_E_SPACE;
}

/* -------------------------------------------------------- firmware container */

enum dchid_status dchid_firmware_parse(const uint8_t *blob, size_t blob_len, uint8_t iface,
                                       uint8_t *patched, size_t cap, size_t *data_len)
{
    uint32_t magic, ver, hdr_len, dlen, ioff;

    if (!blob || !patched || !data_len || blob_len < FW_HDR_SIZE)
        return DCHID_E_ARG;
    magic = rd32(blob);
    ver = rd32(blob + 4);
    hdr_len = rd32(blob + 8);
    dlen = rd32(blob + 12);
    ioff = rd32(blob + 16);
    if (magic != DCHID_FW_MAGIC || ver != DCHID_FW_VER || hdr_len < FW_HDR_SIZE ||
        hdr_len > blob_len || (size_t)hdr_len + dlen > blob_len || ioff >= dlen)
        return DCHID_E_FIRMWARE;
    if (cap < dlen)
        return DCHID_E_SPACE;
    memcpy(patched, blob + hdr_len, dlen);
    if (ioff)
        patched[ioff] = iface;
    *data_len = dlen;
    return DCHID_OK;
}
