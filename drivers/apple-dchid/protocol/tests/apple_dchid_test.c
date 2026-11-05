/* SPDX-License-Identifier: MIT */
/* Host tests for the DockChannel HID protocol core. Build:
 *   cc -std=c11 -Wall -Wextra -fsanitize=address,undefined \
 *      -Iinclude src/apple_dchid.c tests/apple_dchid_test.c -o dchid_test && ./dchid_test
 */
#include "apple_dchid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
    fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- helpers that play the MTP side ---------------------------------------- */

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

/* Build a packet the way the MTP firmware would (any channel). */
static size_t mtp_packet(uint8_t channel, uint8_t seq, uint8_t iface, uint8_t flags,
                         uint32_t retcode, const uint8_t *payload, size_t len, uint8_t *out)
{
    size_t padded = (len + 3) & ~(size_t)3, body = 8 + padded;
    memset(out, 0, 8 + body + 4);
    out[0] = 8; out[1] = channel; put16(out + 2, (uint16_t)body); out[4] = seq; out[5] = iface;
    out[8] = flags; put16(out + 10, (uint16_t)len); put32(out + 12, retcode);
    memcpy(out + 16, payload, len);
    put32(out + 8 + body, 0xffffffffu - dchid_checksum(out, 8 + body));
    return 8 + body + 4;
}

static size_t mtp_event(uint8_t iface, const uint8_t *payload, size_t len, uint8_t *out)
{
    return mtp_packet(DCHID_CHANNEL_REPORT, 0, iface, DCHID_FLAGS(DCHID_TYPE_INPUT, 0), 0,
                      payload, len, out);
}

/* INIT event for interface `idx`: optional HID descriptor and GPIO request. */
static size_t init_event(uint8_t idx, const char *name, const uint8_t *desc, uint16_t desc_len,
                         uint16_t gpio_id, const char *gpio_name, bool more, uint8_t *out)
{
    uint8_t p[512];
    size_t n = 0;
    memset(p, 0, sizeof(p));
    p[0] = DCHID_EVENT_INIT; p[3] = idx;
    strncpy((char *)p + 4, name, 16);
    p[20] = more;
    n = 22;
    if (desc_len) {
        put16(p + n, DCHID_INIT_HID_DESCRIPTOR); put16(p + n + 2, desc_len); n += 4;
        memcpy(p + n, desc, desc_len); n += desc_len;
    }
    if (gpio_id) {
        put16(p + n, DCHID_INIT_GPIO_REQUEST); put16(p + n + 2, 36); n += 4;
        put16(p + n + 2, gpio_id); strncpy((char *)p + n + 4, gpio_name, 32); n += 36;
    }
    put16(p + n, DCHID_INIT_TERMINATOR); put16(p + n + 2, 0); n += 4;
    return mtp_event(DCHID_IFACE_COMM, p, n, out);
}

static size_t feed(struct dchid_session *s, const uint8_t *pkt, size_t len,
                   struct dchid_event *ev, size_t max, enum dchid_status *st)
{
    struct dchid_packet p;
    size_t n = 0;
    *st = dchid_decode(pkt, len, &p);
    if (*st == DCHID_OK)
        *st = dchid_session_handle(s, &p, ev, max, &n);
    return n;
}

/* ---- tests -------------------------------------------------------------------- */

static void test_encode_decode(void)
{
    uint8_t msg[3] = {0xb4, 0x05, 0x77}, pkt[64];
    size_t len;
    struct dchid_packet p;

    CHECK(dchid_encode(0, 9, DCHID_FLAGS(DCHID_TYPE_FEATURE, DCHID_REQ_SET_REPORT),
                       msg, sizeof(msg), pkt, sizeof(pkt), &len) == DCHID_OK);
    CHECK(len == 8 + 8 + 4 + 4);            /* hdr + sub + padded(3) + checksum */
    CHECK(pkt[0] == 8 && pkt[1] == DCHID_CHANNEL_CMD && pkt[4] == 9);
    CHECK(pkt[2] == 12 && pkt[3] == 0);     /* body = sub(8) + 4 */
    CHECK(pkt[8] == 0x80);                  /* feature << 6 | set_report */
    CHECK(pkt[10] == 3);                    /* unpadded length */
    CHECK(dchid_checksum(pkt, len) == 0xffffffffu);
    CHECK(dchid_decode(pkt, len, &p) == DCHID_OK);
    CHECK(p.payload_len == 3 && !memcmp(p.payload, msg, 3) && p.seq == 9);

    pkt[17] ^= 1;
    CHECK(dchid_decode(pkt, len, &p) == DCHID_E_CHECKSUM);
    pkt[17] ^= 1;
    CHECK(dchid_decode(pkt, len - 1, &p) == DCHID_E_HEADER);
    CHECK(dchid_encode(0, 0, 0, msg, 3, pkt, 10, &len) == DCHID_E_SPACE);
}

static void test_stream(void)
{
    struct dchid_rx_stream rx;
    struct dchid_packet p;
    uint8_t a[64], b[64], stream[256], kb[] = {0x01, 0x00, 0x04, 0, 0, 0, 0, 0, 0};
    size_t la = mtp_event(3, kb, sizeof(kb), a);
    size_t lb = mtp_event(4, kb, 4, b);
    size_t n = 0, i, got = 0;
    uint8_t garbage[] = {0x55, 0x08, 0xaa, 0x13};

    memcpy(stream + n, garbage, sizeof(garbage)); n += sizeof(garbage);
    memcpy(stream + n, a, la); n += la;
    memcpy(stream + n, b, lb); n += lb;

    dchid_rx_reset(&rx);
    for (i = 0; i < n;) {                     /* one byte at a time, like a slow FIFO */
        i += dchid_rx_push(&rx, stream + i, 1);
        while (dchid_rx_peek(&rx, &p) == DCHID_OK) {
            got++;
            CHECK(p.iface == (got == 1 ? 3 : 4));
            dchid_rx_pop(&rx);
        }
    }
    CHECK(got == 2);
    CHECK(rx.dropped >= 1);                  /* the garbage was skipped */

    /* A plausible-looking header with a bad checksum, then a good packet. */
    dchid_rx_reset(&rx);
    memcpy(stream, a, la); stream[20] ^= 0xff;
    memcpy(stream + la, b, lb);
    got = 0;
    for (i = 0; i < la + lb;) {
        enum dchid_status st;
        i += dchid_rx_push(&rx, stream + i, la + lb - i);
        while ((st = dchid_rx_peek(&rx, &p)) != DCHID_NEED_MORE) {
            if (st == DCHID_OK) { got++; CHECK(p.iface == 4); dchid_rx_pop(&rx); }
        }
    }
    CHECK(got == 1);
}

static void test_bringup(void)
{
    struct dchid_session s;
    struct dchid_event ev[8];
    enum dchid_status st;
    uint8_t pkt[1024], desc[] = {0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0xc0};
    size_t len, n;

    dchid_session_init(&s);

    /* keyboard arrives before stm: must be deferred */
    len = init_event(3, "keyboard", desc, sizeof(desc), 0, NULL, false, pkt);
    n = feed(&s, pkt, len, ev, 8, &st);
    CHECK(st == DCHID_OK && n == 0);
    CHECK(s.ifaces[3].deferred && s.ifaces[3].hid_desc_len == sizeof(desc));

    /* multi-touch in two INIT packets, with a GPIO request in the second */
    len = init_event(4, "multi-touch", desc, sizeof(desc), 0, NULL, true, pkt);
    n = feed(&s, pkt, len, ev, 8, &st);
    CHECK(st == DCHID_OK && n == 0 && !s.ifaces[4].init_complete);
    len = init_event(4, "multi-touch", NULL, 0, 7, "tp-reset", false, pkt);
    n = feed(&s, pkt, len, ev, 8, &st);
    CHECK(st == DCHID_OK && n == 0 && s.ifaces[4].gpio_id == 7);
    CHECK(!strcmp(s.ifaces[4].gpio_name, "tp-reset"));

    /* stm: enabled immediately */
    len = init_event(1, "stm", desc, sizeof(desc), 0, NULL, false, pkt);
    n = feed(&s, pkt, len, ev, 8, &st);
    CHECK(st == DCHID_OK && n == 1 && ev[0].kind == DCHID_EV_ENABLE_IFACE && ev[0].iface == 1);

    /* stm ready -> ready, stm_ready, then both deferred interfaces get enabled */
    {
        uint8_t r[2] = {DCHID_EVENT_READY, 1};
        len = mtp_event(DCHID_IFACE_COMM, r, 2, pkt);
        n = feed(&s, pkt, len, ev, 8, &st);
        CHECK(st == DCHID_OK && n == 4);
        CHECK(ev[0].kind == DCHID_EV_IFACE_READY && ev[1].kind == DCHID_EV_STM_READY);
        CHECK(ev[2].kind == DCHID_EV_ENABLE_IFACE && ev[2].iface == 3);
        CHECK(ev[3].kind == DCHID_EV_ENABLE_IFACE && ev[3].iface == 4);
    }
    CHECK(dchid_session_find(&s, "keyboard") == &s.ifaces[3]);

    /* a key press on the keyboard interface */
    {
        uint8_t key[] = {0x01, 0x00, 0x00, 0x04};
        len = mtp_event(3, key, sizeof(key), pkt);
        n = feed(&s, pkt, len, ev, 8, &st);
        CHECK(st == DCHID_OK && n == 1 && ev[0].kind == DCHID_EV_INPUT_REPORT);
        CHECK(ev[0].iface == 3 && ev[0].len == 4 && ev[0].data[3] == 0x04);
    }
}

static void test_command_ack(void)
{
    struct dchid_session s;
    struct dchid_event ev[4];
    enum dchid_status st;
    uint8_t msg[2], out[64], pkt[64], ackp[8];
    size_t mlen = dchid_payload_enable(3, msg), olen, len, n;
    uint8_t flags = DCHID_FLAGS(DCHID_TYPE_FEATURE, DCHID_REQ_SET_REPORT);

    dchid_session_init(&s);
    CHECK(dchid_session_command(&s, 0, DCHID_TYPE_FEATURE, DCHID_REQ_SET_REPORT,
                                msg, mlen, out, sizeof(out), &olen) == DCHID_OK);
    CHECK(dchid_session_command(&s, 0, DCHID_TYPE_FEATURE, DCHID_REQ_SET_REPORT,
                                msg, mlen, out, sizeof(out), &olen) == DCHID_E_BUSY);

    /* wrong sequence number is rejected and the command stays outstanding */
    ackp[0] = DCHID_CMD_ENABLE_INTERFACE;
    len = mtp_packet(DCHID_CHANNEL_CMD, 5, 0, flags, 0, ackp, 1, pkt);
    n = feed(&s, pkt, len, ev, 4, &st);
    CHECK(st == DCHID_E_UNEXPECTED && n == 0);

    len = mtp_packet(DCHID_CHANNEL_CMD, 0, 0, flags, 0, ackp, 1, pkt);
    n = feed(&s, pkt, len, ev, 4, &st);
    CHECK(st == DCHID_OK && n == 1 && ev[0].kind == DCHID_EV_CMD_DONE && ev[0].retcode == 0);
    CHECK(s.ifaces[0].tx_seq == 1 && s.ifaces[0].out_report == -1);

    /* GET_REPORT with a response body; report id is stripped */
    {
        uint8_t q = DCHID_STM_REPORT_ID, resp[4] = {DCHID_STM_REPORT_ID, 0xac, 0x05, 0x42};
        uint8_t gf = DCHID_FLAGS(DCHID_TYPE_FEATURE, DCHID_REQ_GET_REPORT);
        CHECK(dchid_session_command(&s, 0, DCHID_TYPE_FEATURE, DCHID_REQ_GET_REPORT,
                                    &q, 1, out, sizeof(out), &olen) == DCHID_OK);
        len = mtp_packet(DCHID_CHANNEL_CMD, 1, 0, gf, 0xe00002c2u, resp, 4, pkt);
        n = feed(&s, pkt, len, ev, 4, &st);
        CHECK(st == DCHID_OK && n == 1 && ev[0].len == 3 && ev[0].data[0] == 0xac);
        CHECK(ev[0].retcode == 0xe00002c2u);
    }

    /* cancel after a timeout advances seq like Linux */
    CHECK(dchid_session_command(&s, 0, DCHID_TYPE_FEATURE, DCHID_REQ_SET_REPORT,
                                msg, mlen, out, sizeof(out), &olen) == DCHID_OK);
    dchid_session_cancel(&s, 0);
    CHECK(s.ifaces[0].tx_seq == 3 && s.ifaces[0].out_report == -1);
}

static void test_payloads_and_gpio(void)
{
    struct dchid_session s;
    struct dchid_event ev[4];
    enum dchid_status st;
    uint8_t fw[16], pkt[512], ack[32], g[5];
    size_t len, n;

    CHECK(dchid_payload_firmware(4, 0x0123456789abcdefull, 0x1000, fw) == 16);
    CHECK(fw[0] == 0x95 && fw[1] == 2 && fw[3] == 4 && fw[4] == 0xef && fw[11] == 0x01);
    CHECK(fw[12] == 0x00 && fw[13] == 0x10);

    dchid_session_init(&s);
    len = init_event(4, "multi-touch", NULL, 0, 7, "tp-reset", false, pkt);
    feed(&s, pkt, len, ev, 4, &st);

    g[0] = DCHID_EVENT_GPIO_CMD; g[1] = 4; g[2] = 7; g[3] = 0; g[4] = 3;
    len = mtp_event(DCHID_IFACE_COMM, g, 5, pkt);
    n = feed(&s, pkt, len, ev, 4, &st);
    CHECK(st == DCHID_OK && n == 1 && ev[0].kind == DCHID_EV_GPIO_PULSE);
    CHECK(ev[0].retcode == 0 && ev[0].gpio_id == 7 && ev[0].gpio_cmd_len == 5);
    CHECK(dchid_payload_gpio_ack(0, ev[0].gpio_cmd, ev[0].gpio_cmd_len, ack, sizeof(ack)) == 10);
    CHECK(ack[0] == 0xa1 && ack[5] == DCHID_EVENT_GPIO_CMD && ack[9] == 3);

    g[2] = 9; /* wrong gpio -> still acked, with an error code */
    len = mtp_event(DCHID_IFACE_COMM, g, 5, pkt);
    n = feed(&s, pkt, len, ev, 4, &st);
    CHECK(n == 1 && ev[0].retcode == 0xe000f00du);
}

static void test_firmware(void)
{
    uint8_t blob[64], out[64];
    size_t dlen;
    memset(blob, 0, sizeof(blob));
    put32(blob, DCHID_FW_MAGIC); put32(blob + 4, 1); put32(blob + 8, 20);
    put32(blob + 12, 16); put32(blob + 16, 2);
    memset(blob + 20, 0xee, 16);
    CHECK(dchid_firmware_parse(blob, 36, 4, out, sizeof(out), &dlen) == DCHID_OK);
    CHECK(dlen == 16 && out[2] == 4 && out[0] == 0xee);
    CHECK(dchid_firmware_parse(blob, 35, 4, out, sizeof(out), &dlen) == DCHID_E_FIRMWARE);
    put32(blob, 0x12345678);
    CHECK(dchid_firmware_parse(blob, 36, 4, out, sizeof(out), &dlen) == DCHID_E_FIRMWARE);
}

static void test_fuzz(void)
{
    struct dchid_rx_stream rx;
    struct dchid_session s;
    struct dchid_packet p;
    struct dchid_event ev[16];
    uint8_t buf[4096];
    uint32_t x = 0x1234567u;
    int round;

    dchid_rx_reset(&rx);
    dchid_session_init(&s);
    for (round = 0; round < 20000; round++) {
        size_t len = (x % 300) + 1, i, used = 0, n;
        for (i = 0; i < len; i++) { x = x * 1103515245u + 12345u; buf[i] = (uint8_t)(x >> 16); }
        if (round % 7 == 0) /* sometimes splice in a valid packet */
            len = init_event((uint8_t)(1 + x % 15), "fuzz", buf, (uint16_t)(x % 64), 0, NULL,
                             x & 1, buf);
        while (used < len) {
            used += dchid_rx_push(&rx, buf + used, len - used);
            while (dchid_rx_peek(&rx, &p) != DCHID_NEED_MORE) {
                if (dchid_decode(rx.buf, 8 + (size_t)(rx.buf[2] | rx.buf[3] << 8) + 4, &p) == DCHID_OK) {
                    dchid_session_handle(&s, &p, ev, 16, &n);
                    dchid_rx_pop(&rx);
                }
            }
        }
    }
    CHECK(rx.fill <= sizeof(rx.buf));
}

int main(void)
{
    test_encode_decode();
    test_stream();
    test_bringup();
    test_command_ack();
    test_payloads_and_gpio();
    test_firmware();
    test_fuzz();
    printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
