/*
 * Host test: what m1n1 hands the J613 Windows guest through the AVHD page.
 * The guest keyboard descriptor must pass the Windows driver's own contract
 * parser, trackpad reports must match the J313-validated decoder, and the
 * report queue must never lose the newest (key-up / finger-up) state.
 */
#define DCHID_HOST_TEST 1
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../m1n1_windows/src/dchid_proto.h"
#include "apple_spihid.h"
#include "apple_mtp.h"
#include "apple_trackpad.h"

static void test_report_size_fixup_matches_linux(void)
{
    /* hid-apple / apple-hid (2026-06): 76 00 40 95 n -> 75 08 96 00 n*8, n in 1..31 */
    u8 d[] = {0x06, 0x00, 0xff, 0x76, 0x00, 0x40, 0x95, 0x02, 0xb1, 0x02,
              0x76, 0x00, 0x40, 0x95, 0x00, 0x76, 0x00, 0x40, 0x95, 0x20};
    assert(dchid_fixup_report_size(d, sizeof(d)) == 1);
    assert(d[3] == 0x75 && d[4] == 0x08 && d[5] == 0x96 && d[6] == 0x00 && d[7] == 16);
    assert(d[10] == 0x76 && d[15] == 0x76); /* count 0 and 32 untouched */
    /* bits preserved: 16384 * 2 == 8 * (16 << 8) */
    assert(16384 * 2 == 8 * (16 << 8));
    assert(dchid_fixup_report_size(d, 4) == 0);
}

static void test_guest_keyboard_descriptor(void)
{
    struct ai_hid_input_contract c;
    unsigned char id = 0;
    u8 r[DCHID_KBD_REPORT_LEN] = {1, 0x08, 0, 0x15, 0, 0, 0, 0, 0, 0}; /* cmd+R from EXP-049 */

    assert(ai_hid_input_contract_parse(dchid_guest_kbd_desc, dchid_guest_kbd_desc_len, &c) ==
           AI_OK);
    assert(c.uses_report_ids && c.bytes_by_id[DCHID_KBD_REPORT_ID] == DCHID_KBD_REPORT_LEN);
    assert(ai_hid_input_report_valid(&c, r, sizeof(r), &id) && id == 1);
    /* Generic Desktop / Keyboard application collection, report ID 1 */
    assert(dchid_guest_kbd_desc[0] == 0x05 && dchid_guest_kbd_desc[1] == 0x01);
    assert(dchid_guest_kbd_desc[2] == 0x09 && dchid_guest_kbd_desc[3] == 0x06);
    assert(dchid_guest_kbd_desc[dchid_guest_kbd_desc_len - 1] == 0xc0);
}

static void test_iso_swap(void)
{
    u8 r[DCHID_KBD_REPORT_LEN] = {1, 0, 0, 0x35, 0x64, 0x04, 0, 0, 0, 0x35};
    dchid_kbd_iso_swap(r);
    assert(r[3] == 0x64 && r[4] == 0x35 && r[5] == 0x04);
    assert(r[9] == 0x35); /* vendor byte untouched */
}

static void make_mt(u8 *p, size_t len, u8 nf, u8 buttons)
{
    memset(p, 0, len);
    p[0] = DCHID_MT_REPORT_ID;
    p[DCHID_MT_NFINGERS_OFFSET] = nf;
    p[DCHID_MT_BUTTONS_OFFSET] = buttons;
    for (size_t i = 0; i < nf && DCHID_MT_HDR_SIZE + (i + 1) * 30 <= len; i++) {
        u8 *f = p + DCHID_MT_HDR_SIZE + i * 30;
        f[4] = 0x10 + i;   /* abs_x */
        f[6] = 0x20 + i;   /* abs_y */
        f[18] = 0x40;      /* touch_major */
    }
}

/* What the Windows driver does with an AVHD trackpad report (transport_avhd.c). */
static void assert_decodes(const u8 *mt, size_t len, size_t contacts, bool button)
{
    u8 spi[DCHID_MT_GUEST_MAX + AI_MTP_SPI_PREFIX_SIZE];
    size_t spi_len = 0;
    struct ai_apple_trackpad_frame frame;

    assert(ai_mtp_trackpad_to_spi(mt, len, spi, sizeof(spi), &spi_len) == AI_OK);
    assert(ai_apple_trackpad_decode(spi, spi_len, &frame) == AI_OK);
    assert(frame.count == contacts && frame.button == button);
}

static void test_trackpad_sanitize(void)
{
    u8 in[DCHID_MT_HDR_SIZE + 16 * 30], out[DCHID_MT_GUEST_MAX];
    size_t n;

    /* exact: unchanged */
    make_mt(in, DCHID_MT_HDR_SIZE + 2 * 30, 2, 1);
    assert(dchid_mt_sanitize(in, DCHID_MT_HDR_SIZE + 2 * 30, out, sizeof(out), &n) == 0);
    assert(n == DCHID_MT_HDR_SIZE + 60);
    assert_decodes(out, n, 2, true);

    /* spare finger slots (Linux: npoints >= num_fingers): trimmed, then decodable */
    make_mt(in, DCHID_MT_HDR_SIZE + 5 * 30, 1, 0);
    assert(dchid_mt_sanitize(in, DCHID_MT_HDR_SIZE + 5 * 30, out, sizeof(out), &n) == 1);
    assert(n == DCHID_MT_HDR_SIZE + 30);
    assert_decodes(out, n, 1, false);

    /* buttons byte with more than bit 0 (Linux uses buttons & 1) */
    make_mt(in, DCHID_MT_HDR_SIZE + 30, 1, 3);
    assert(dchid_mt_sanitize(in, DCHID_MT_HDR_SIZE + 30, out, sizeof(out), &n) == 0);
    assert(out[DCHID_MT_BUTTONS_OFFSET] == 1);
    assert_decodes(out, n, 1, true);

    /* more fingers than the decoder takes: capped at 11 */
    make_mt(in, sizeof(in), 16, 0);
    assert(dchid_mt_sanitize(in, sizeof(in), out, sizeof(out), &n) == 1);
    assert(out[DCHID_MT_NFINGERS_OFFSET] == 11 && n == DCHID_MT_GUEST_MAX);
    assert_decodes(out, n, 11, false);

    /* zero fingers (lift-off frame) */
    make_mt(in, DCHID_MT_HDR_SIZE, 0, 0);
    assert(dchid_mt_sanitize(in, DCHID_MT_HDR_SIZE, out, sizeof(out), &n) == 0);
    assert_decodes(out, n, 0, false);

    /* malformed: fewer slots than num_fingers, too short */
    make_mt(in, DCHID_MT_HDR_SIZE + 30, 3, 0);
    assert(dchid_mt_sanitize(in, DCHID_MT_HDR_SIZE + 30, out, sizeof(out), &n) < 0);
    assert(dchid_mt_sanitize(in, 10, out, sizeof(out), &n) < 0);
}

static void push1(struct dchid_vq *q, u8 iface, u8 v)
{
    dchid_vq_push(q, iface, &v, 1);
}

static void test_queue(void)
{
    struct dchid_vq q;
    const struct dchid_vq_slot *s;

    memset(&q, 0, sizeof(q));
    q.ring[0].cap = 4;
    q.ring[0].slot = calloc(4, sizeof(struct dchid_vq_slot));
    q.ring[1].cap = 3;
    q.ring[1].slot = calloc(3, sizeof(struct dchid_vq_slot));

    assert(!dchid_vq_latch(&q));
    /* global order across both rings */
    push1(&q, 0, 1);
    push1(&q, 1, 2);
    push1(&q, 0, 3);
    assert(q.count == 3);
    s = dchid_vq_latch(&q);
    assert(s && s->iface == 0 && s->data[0] == 1);
    assert(dchid_vq_latch(&q) == s); /* stable until popped */
    dchid_vq_pop(&q);
    s = dchid_vq_latch(&q);
    assert(s->iface == 1 && s->data[0] == 2);
    dchid_vq_pop(&q);
    s = dchid_vq_latch(&q);
    assert(s->iface == 0 && s->data[0] == 3);
    dchid_vq_pop(&q);
    assert(q.count == 0 && !dchid_vq_latch(&q));

    /* trackpad flood never evicts keyboard reports; overflow drops the OLDEST */
    push1(&q, 0, 10); /* key down */
    for (u8 i = 0; i < 20; i++)
        push1(&q, 1, 100 + i);
    push1(&q, 0, 11); /* key up: must survive */
    assert(q.drops == 17 && q.count == 5);
    s = dchid_vq_latch(&q);
    assert(s->iface == 0 && s->data[0] == 10);
    dchid_vq_pop(&q);
    s = dchid_vq_latch(&q);
    assert(s->iface == 1 && s->data[0] == 117); /* newest 3 trackpad frames kept */
    dchid_vq_pop(&q);
    dchid_vq_pop(&q); /* pop without latch: no-op */
    s = dchid_vq_latch(&q);
    assert(s->data[0] == 118);
    dchid_vq_pop(&q);
    s = dchid_vq_latch(&q);
    assert(s->data[0] == 119);
    dchid_vq_pop(&q);
    s = dchid_vq_latch(&q);
    assert(s->iface == 0 && s->data[0] == 11);
    dchid_vq_pop(&q);
    assert(q.count == 0);

    /* latched report evicted while the guest reads it: no tearing, pop removes nothing else */
    push1(&q, 1, 1);
    s = dchid_vq_latch(&q);
    assert(s->data[0] == 1);
    push1(&q, 1, 2);
    push1(&q, 1, 3);
    push1(&q, 1, 4); /* evicts 1 */
    assert(s->data[0] == 1 && q.count == 3);
    dchid_vq_pop(&q);
    assert(q.count == 3);
    s = dchid_vq_latch(&q);
    assert(s->data[0] == 2);

    /* flush on driver start */
    dchid_vq_flush(&q);
    assert(q.count == 0 && !dchid_vq_latch(&q));
    push1(&q, 0, 7);
    s = dchid_vq_latch(&q);
    assert(s->data[0] == 7);
    free(q.ring[0].slot);
    free(q.ring[1].slot);
}

int main(void)
{
    test_report_size_fixup_matches_linux();
    test_guest_keyboard_descriptor();
    test_iso_swap();
    test_trackpad_sanitize();
    test_queue();
    puts("all tests passed");
    return 0;
}
