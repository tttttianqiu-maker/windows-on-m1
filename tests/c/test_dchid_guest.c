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
#include "apple_precision_touchpad_descriptor.h"

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
    assert(c.bytes_by_id[DCHID_CONSUMER_REPORT_ID] == DCHID_CONSUMER_REPORT_LEN);
    {
        u8 cr[DCHID_CONSUMER_REPORT_LEN] = {2, 0xe9, 0, 0, 0};
        assert(ai_hid_input_report_valid(&c, cr, sizeof(cr), &id) && id == 2);
    }
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

/* Shaped like Apple keyboard descriptors (Magic Keyboard / MTP keyboard). */
static const u8 apple_like_kbd[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01,
    0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, /* LEDs: output */
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x26, 0xff, 0x00, 0x05, 0x07, 0x19, 0x00, 0x2a, 0xff, 0x00,
    0x81, 0x00,
    0x05, 0x0c, 0x75, 0x01, 0x95, 0x01, 0x09, 0xb8, 0x15, 0x00, 0x25, 0x01, 0x81, 0x02, /* eject */
    0x05, 0xff, 0x09, 0x03, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02,                         /* Fn */
    0x75, 0x06, 0x95, 0x01, 0x81, 0x01,
    0xc0,
    /* vendor collection with an oversized feature report (fixed up first) */
    0x06, 0x00, 0xff, 0x09, 0x0f, 0xa1, 0x01, 0x85, 0x09, 0x09, 0x0f, 0x76, 0x00, 0x40, 0x95, 0x02,
    0xb1, 0x02, 0xc0,
};

static void test_hid_scan(void)
{
    u8 d[sizeof(apple_like_kbd)];
    u32 bits;
    int fn;

    memcpy(d, apple_like_kbd, sizeof(d));
    assert(dchid_fixup_report_size(d, sizeof(d)) == 1);
    assert(dchid_hid_scan(d, sizeof(d), 1, &bits, &fn));
    assert(bits == 72 && 1 + (bits + 7) / 8 == DCHID_KBD_REPORT_LEN);
    assert(fn == 65); /* byte 9, bit 1 */
    /* 4-byte usage form of the Fn usage on page 0xff00 */
    const u8 d2[] = {0x85, 0x01, 0x0b, 0x03, 0x00, 0x00, 0xff, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02,
                     0x75, 0x07, 0x81, 0x01};
    assert(dchid_hid_scan(d2, sizeof(d2), 1, &bits, &fn) && fn == 0 && bits == 8);
    /* no Fn usage */
    assert(dchid_hid_scan(dchid_guest_kbd_desc, dchid_guest_kbd_desc_len, 1, &bits, &fn));
    assert(fn == -1 && bits == 72);
    /* other report ids do not count */
    assert(dchid_hid_scan(d, sizeof(d), 9, &bits, &fn) && bits == 0 && fn == -1);
    /* truncated item, pop without push */
    const u8 bad1[] = {0x05};
    const u8 bad2[] = {0xb4};
    assert(!dchid_hid_scan(bad1, sizeof(bad1), 1, &bits, &fn));
    assert(!dchid_hid_scan(bad2, sizeof(bad2), 1, &bits, &fn));
}

static void kbd(u8 *r, u8 k0, u8 k1)
{
    memset(r, 0, DCHID_KBD_REPORT_LEN);
    r[0] = 1;
    r[3] = k0;
    r[4] = k1;
}

static void test_fn_layer(void)
{
    struct dchid_fn_state st;
    u8 r[DCHID_KBD_REPORT_LEN];

    memset(&st, 0, sizeof(st));
    kbd(r, 0x2a, 0); /* Backspace alone */
    dchid_kbd_fn_translate(&st, r, false);
    assert(r[3] == 0x2a);
    kbd(r, 0, 0);
    dchid_kbd_fn_translate(&st, r, false);

    kbd(r, 0x2a, 0); /* Fn + Backspace = Delete */
    dchid_kbd_fn_translate(&st, r, true);
    assert(r[3] == 0x4c);
    kbd(r, 0x2a, 0); /* Fn released, Backspace still held: stays Delete */
    dchid_kbd_fn_translate(&st, r, false);
    assert(r[3] == 0x4c);
    kbd(r, 0x2a, 0x50); /* + Left pressed without Fn: plain Left */
    dchid_kbd_fn_translate(&st, r, false);
    assert(r[3] == 0x4c && r[4] == 0x50);
    kbd(r, 0, 0);
    dchid_kbd_fn_translate(&st, r, false);
    kbd(r, 0x2a, 0); /* after release, Backspace is Backspace again */
    dchid_kbd_fn_translate(&st, r, false);
    assert(r[3] == 0x2a);
    kbd(r, 0, 0);
    dchid_kbd_fn_translate(&st, r, false);

    kbd(r, 0x52, 0x51); /* Fn+Up/Down = PgUp/PgDn */
    dchid_kbd_fn_translate(&st, r, true);
    assert(r[3] == 0x4b && r[4] == 0x4e);
    kbd(r, 0x50, 0x4f); /* Fn+Left/Right = Home/End (new presses, Fn still held) */
    dchid_kbd_fn_translate(&st, r, true);
    assert(r[3] == 0x4a && r[4] == 0x4d);
    kbd(r, 0x28, 0x04); /* Fn+Enter = Insert, letters unchanged */
    dchid_kbd_fn_translate(&st, r, true);
    assert(r[3] == 0x49 && r[4] == 0x04);
    kbd(r, 0x01, 0); /* rollover error code untouched */
    dchid_kbd_fn_translate(&st, r, true);
    assert(r[3] == 0x01);
}

static void le16(u8 *p, int v)
{
    p[0] = (u8)(v & 0xff);
    p[1] = (u8)((v >> 8) & 0xff);
}

/*
 * The whole Windows trackpad path with the J613's real dimensions
 * (EXP-20261108-045: 12194 x 7408 1/100 mm, x -5787..6256, y -157..7102):
 * dims -> axis contract -> PTP descriptor patch, and a sanitized MTP frame ->
 * decode -> tracker -> PTP input report.
 */
static void test_j613_trackpad_pipeline(void)
{
    u8 dims[16];
    struct ai_trackpad_dimensions dim;
    struct ai_trackpad_axis_contract axes;
    static u8 ptp_desc[AI_PTP_DESCRIPTOR_SIZE];
    u8 in[DCHID_MT_HDR_SIZE + 3 * 30], mt[DCHID_MT_GUEST_MAX];
    u8 spi[DCHID_MT_GUEST_MAX + AI_MTP_SPI_PREFIX_SIZE];
    size_t n, spi_len, ptp_len;
    struct ai_apple_trackpad_frame frame;
    static struct ai_trackpad_tracker tracker;
    struct ai_trackpad_output_frame out;
    struct ai_ptp_feature_state features;
    u8 report[64];

    dims[0] = 12194 & 0xff; dims[1] = 12194 >> 8; dims[2] = dims[3] = 0;
    dims[4] = 7408 & 0xff; dims[5] = 7408 >> 8; dims[6] = dims[7] = 0;
    le16(dims + 8, -5787);
    le16(dims + 10, -157);
    le16(dims + 12, 6256);
    le16(dims + 14, 7102);
    assert(ai_mtp_dimensions_parse(dims, &dim) == AI_OK);
    assert(ai_trackpad_axis_contract_from_dimensions(&dim, &axes) == AI_OK);
    assert(AiPrecisionTouchpadDescriptorPatch(ptp_desc, sizeof(ptp_desc), &axes));

    /* one finger in the middle, report with two spare slots (trimmed by m1n1) */
    make_mt(in, sizeof(in), 1, 0);
    le16(in + DCHID_MT_HDR_SIZE + 4, 234);  /* abs_x */
    le16(in + DCHID_MT_HDR_SIZE + 6, 3472); /* abs_y */
    assert(dchid_mt_sanitize(in, sizeof(in), mt, sizeof(mt), &n) == 1);
    assert(ai_mtp_trackpad_to_spi(mt, n, spi, sizeof(spi), &spi_len) == AI_OK);
    assert(ai_apple_trackpad_decode(spi, spi_len, &frame) == AI_OK && frame.count == 1);
    memset(&tracker, 0, sizeof(tracker));
    assert(ai_trackpad_tracker_update(&tracker, &frame, &out) == AI_OK);
    ai_ptp_feature_init(&features);
    features.input_mode = 3; /* what Windows sets through SET_FEATURE */
    assert(ai_ptp_encode_input(&axes, &out, 100, &features, report, sizeof(report), &ptp_len) ==
           AI_OK);
    assert(ptp_len > 0);
}

static void test_fn_media(void)
{
    struct dchid_fn_state st;
    u8 r[DCHID_KBD_REPORT_LEN], c[DCHID_CONSUMER_REPORT_LEN];

    memset(&st, 0, sizeof(st));
    kbd(r, 0x45, 0x04); /* F12 alone (+ 'a'): stays F12, no consumer report */
    dchid_kbd_fn_translate(&st, r, false);
    assert(!dchid_kbd_fn_media(&st, r, c));
    assert(r[3] == 0x45 && r[4] == 0x04);
    kbd(r, 0, 0);
    dchid_kbd_fn_translate(&st, r, false);
    assert(!dchid_kbd_fn_media(&st, r, c));

    kbd(r, 0x04, 0x45); /* 'a' held, then Fn+F12 = Volume Up, F12 leaves report 1 */
    dchid_kbd_fn_translate(&st, r, true);
    assert(dchid_kbd_fn_media(&st, r, c));
    assert(c[0] == 2 && c[1] == 0xe9 && c[2] == 0 && c[3] == 0 && c[4] == 0);
    assert(r[3] == 0x04 && r[4] == 0);
    kbd(r, 0x04, 0x45); /* same state again: no new consumer report */
    dchid_kbd_fn_translate(&st, r, false); /* Fn released, F12 still held: still volume */
    assert(!dchid_kbd_fn_media(&st, r, c));
    assert(r[3] == 0x04 && r[4] == 0);
    kbd(r, 0x04, 0); /* F12 released: consumer report goes empty */
    dchid_kbd_fn_translate(&st, r, false);
    assert(dchid_kbd_fn_media(&st, r, c) && c[1] == 0 && c[2] == 0);

    kbd(r, 0x41, 0x3c); /* Fn+F8 = Play/Pause, Fn+F3 stays F3 */
    dchid_kbd_fn_translate(&st, r, true);
    assert(dchid_kbd_fn_media(&st, r, c) && c[1] == 0xcd && c[2] == 0x00);
    assert(r[3] == 0x3c && r[4] == 0);
    kbd(r, 0x3d, 0x3a); /* Fn+F4 = Search (0x221), Fn+F1 = Brightness Down */
    dchid_kbd_fn_translate(&st, r, true);
    assert(dchid_kbd_fn_media(&st, r, c));
    assert(c[1] == 0x21 && c[2] == 0x02 && c[3] == 0x70 && c[4] == 0x00);
    assert(r[3] == 0 && r[4] == 0);
}

static void test_battery_regs(void)
{
    struct dchid_batt_raw r;
    u32 regs[DCHID_BATT_NREGS];

    /* nothing read: magic only, not present (ACPI hides BAT0/ADP1) */
    memset(&r, 0, sizeof(r));
    dchid_batt_regs(&r, 1, regs);
    assert(regs[DCHID_BATT_MAG] == DCHID_BATT_MAGIC && regs[DCHID_BATT_FLG] == 0);

    /* on battery, 62 %, 3 cells, discharging 850 mA */
    memset(&r, 0, sizeof(r));
    r.ok_buic = r.ok_fcc = r.ok_dcap = r.ok_volt = r.ok_cur = r.ok_cyc = r.ok_chis =
        r.ok_cells = true;
    r.buic = 62;
    r.fcc = 4200;
    r.dcap = 4400;
    r.volt = 11850;
    r.cur = -850;
    r.cyc = 42;
    r.chis = 0;
    r.cells = 3;
    dchid_batt_regs(&r, 7, regs);
    assert(regs[DCHID_BATT_FLG] == (DCHID_BATT_PRESENT | DCHID_BATT_DISCHARGING));
    assert(regs[DCHID_BATT_RCAP] == 2604 && regs[DCHID_BATT_FCAP] == 4200);
    assert(regs[DCHID_BATT_DCAP] == 4400 && regs[DCHID_BATT_RATE] == 850);
    assert(regs[DCHID_BATT_DVOLT] == 11400 && regs[DCHID_BATT_CYC] == 42);
    assert(regs[DCHID_BATT_PCT] == 62 && regs[DCHID_BATT_SEQ] == 7);

    /* charger in, charging */
    r.chis = 1;
    r.cur = 1500;
    dchid_batt_regs(&r, 8, regs);
    assert(regs[DCHID_BATT_FLG] == (DCHID_BATT_PRESENT | DCHID_BATT_AC | DCHID_BATT_CHARGING));

    /* charger in, full (no current): neither charging nor discharging */
    r.cur = 0;
    r.buic = 100;
    dchid_batt_regs(&r, 9, regs);
    assert(regs[DCHID_BATT_FLG] == (DCHID_BATT_PRESENT | DCHID_BATT_AC));
    assert(regs[DCHID_BATT_RCAP] == 4200);

    /* critical when discharging at <= 5 %, unknown current, FCC missing -> design */
    r.chis = 0;
    r.ok_cur = false;
    r.ok_fcc = false;
    r.buic = 4;
    dchid_batt_regs(&r, 10, regs);
    assert(regs[DCHID_BATT_FLG] ==
           (DCHID_BATT_PRESENT | DCHID_BATT_DISCHARGING | DCHID_BATT_CRITICAL));
    assert(regs[DCHID_BATT_FCAP] == 4400 && regs[DCHID_BATT_RATE] == 0xffffffff);

    /* the guest view across refreshes: bad reads never replace a good snapshot */
    struct dchid_batt b;
    memset(&b, 0, sizeof(b));
    /* CHIS unknown before the first good read: AC, so 3 % is not critical */
    r.ok_cur = r.ok_fcc = true;
    r.cur = -400;
    r.ok_chis = false;
    r.buic = 3;
    assert(dchid_batt_update(&b, &r) && b.seq == 1);
    assert(b.regs[DCHID_BATT_FLG] == (DCHID_BATT_PRESENT | DCHID_BATT_AC | DCHID_BATT_DISCHARGING));
    /* on battery, 62 % */
    r.ok_chis = true;
    r.chis = 0;
    r.buic = 62;
    assert(!dchid_batt_update(&b, &r) && b.held == 1); /* 3 -> 62: held back */
    assert(!dchid_batt_update(&b, &r) && b.held == 2);
    assert(dchid_batt_update(&b, &r) && b.held == 0 && b.seq == 2);
    assert(b.regs[DCHID_BATT_FLG] == (DCHID_BATT_PRESENT | DCHID_BATT_DISCHARGING));
    assert(b.regs[DCHID_BATT_PCT] == 62 && b.regs[DCHID_BATT_SEQ] == 2);
    /* BUIC read failed: Windows keeps 62 %, never an all-zero battery */
    r.ok_buic = false;
    assert(!dchid_batt_update(&b, &r));
    assert(b.regs[DCHID_BATT_PCT] == 62 && b.regs[DCHID_BATT_RCAP] == 2604 && b.seq == 2);
    assert(b.regs[DCHID_BATT_FLG] & DCHID_BATT_PRESENT);
    /* CHIS failed later: the last AC state (battery) stays */
    r.ok_buic = true;
    r.ok_chis = false;
    r.chis = 1;
    r.buic = 61;
    assert(dchid_batt_update(&b, &r) && b.held == 0 && b.seq == 3);
    assert(b.regs[DCHID_BATT_FLG] == (DCHID_BATT_PRESENT | DCHID_BATT_DISCHARGING));
    /* a sudden 0 % is held back twice, then believed (critical on battery) */
    r.ok_chis = true;
    r.chis = 0;
    r.buic = 0;
    assert(!dchid_batt_update(&b, &r) && !dchid_batt_update(&b, &r));
    assert(b.regs[DCHID_BATT_PCT] == 61);
    assert(dchid_batt_update(&b, &r) && b.regs[DCHID_BATT_PCT] == 0);
    assert(b.regs[DCHID_BATT_FLG] & DCHID_BATT_CRITICAL);
    /* small steps pass at once */
    r.chis = 1;
    r.cur = 2000;
    r.buic = 15;
    assert(dchid_batt_update(&b, &r) && b.regs[DCHID_BATT_PCT] == 15);
    assert(b.regs[DCHID_BATT_FLG] == (DCHID_BATT_PRESENT | DCHID_BATT_AC | DCHID_BATT_CHARGING));

    /* the guest reads the block at DCHID_BATT_BASE, below the descriptor window */
    assert(DCHID_BATT_BASE + 4 * DCHID_BATT_NREGS <= 0x400 && DCHID_BATT_BASE >= 0x50);
}

int main(void)
{
    test_battery_regs();
    test_fn_media();
    test_j613_trackpad_pipeline();
    test_hid_scan();
    test_fn_layer();
    test_report_size_fixup_matches_linux();
    test_guest_keyboard_descriptor();
    test_iso_swap();
    test_trackpad_sanitize();
    test_queue();
    puts("all tests passed");
    return 0;
}
