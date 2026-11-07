/* Host test for m1n1_windows/src/dchid_proto.c (J613 MTP keyboard/trackpad). */
#define DCHID_HOST_TEST 1
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../m1n1_windows/src/dchid_proto.h"

static void test_build_matches_uboot_enable(void)
{
    /* U-Boot dchid_comm_set_report({0xb4, iface}) with tx_seq 5, iface 3. */
    u8 msg[2] = {DCHID_CMD_ENABLE_IFACE, 3};
    u8 out[64];
    size_t n = dchid_build(out, sizeof(out), DCHID_CHANNEL_CMD, DCHID_IFACE_COMM, 5,
                           DCHID_FLAGS(DCHID_TYPE_FEATURE, DCHID_REQ_SET), msg, sizeof(msg));
    assert(n == 8 + 8 + 4 + 4);
    assert(out[0] == 8 && out[1] == 0x11 && out[2] == 12 && out[3] == 0);
    assert(out[4] == 5 && out[5] == 0);
    assert(out[8] == 0x80 && out[10] == 2 && out[11] == 0);
    assert(out[16] == 0xb4 && out[17] == 3 && out[18] == 0 && out[19] == 0);
    assert(dchid_sum(out, n) == 0xffffffff);
    struct dchid_hdr hdr;
    memcpy(&hdr, out, sizeof(hdr));
    assert(dchid_packet_valid(&hdr, out + 8));
    out[17] ^= 1;
    assert(!dchid_packet_valid(&hdr, out + 8));
}

static void test_build_odd_length_pads(void)
{
    u8 msg[9] = {0x40, 2, 4, 0, 0};
    u8 out[64];
    size_t n = dchid_build(out, sizeof(out), DCHID_CHANNEL_CMD, 0, 1, 0x80, msg, sizeof(msg));
    assert(n == 8 + 8 + 12 + 4);
    assert(out[2] == 8 + 12);
    assert(dchid_sum(out, n) == 0xffffffff);
    assert(dchid_build(out, 16, DCHID_CHANNEL_CMD, 0, 1, 0x80, msg, sizeof(msg)) == 0);
}

static void test_parse_init_keyboard_with_descriptor_and_gpio(void)
{
    u8 p[256];
    size_t o = 0;
    memset(p, 0, sizeof(p));
    p[o++] = DCHID_EVENT_INIT; p[o++] = 0; p[o++] = 0; p[o++] = 2;
    memcpy(p + o, "multi-touch", 11); o += 16;
    p[o++] = 0; /* more_packets */ p[o++] = 0;
    /* descriptor block */
    p[o++] = 0; p[o++] = 0; p[o++] = 4; p[o++] = 0;
    p[o++] = 0x05; p[o++] = 0x01; p[o++] = 0x09; p[o++] = 0x06;
    /* gpio block: unk, id=7, name */
    p[o++] = 1; p[o++] = 0; p[o++] = 36; p[o++] = 0;
    p[o++] = 0; p[o++] = 0; p[o++] = 7; p[o++] = 0;
    memcpy(p + o, "afe-reset", 9); o += 32;
    /* terminator */
    p[o++] = 2; p[o++] = 0; p[o++] = 0; p[o++] = 0;
    struct dchid_init_info info;
    assert(dchid_parse_init(p, o, &info));
    assert(info.iface == 2 && !strcmp(info.name, "multi-touch") && !info.more_packets);
    assert(info.descriptor_len == 4 && info.descriptor[0] == 0x05 && info.descriptor[3] == 0x06);
    assert(info.gpio_requested && info.gpio_id == 7 && !strcmp(info.gpio_name, "afe-reset"));
    assert(!dchid_parse_init(p, 10, &info));
    p[0] = 0xf1;
    assert(!dchid_parse_init(p, o, &info));
}

static void test_fw_and_power(void)
{
    u8 blob[64];
    memset(blob, 0, sizeof(blob));
    u32 h[5] = {DCHID_FW_MAGIC, 1, 20, 40, 3};
    memcpy(blob, h, sizeof(h));
    const u8 *data; u32 len, ioff;
    assert(dchid_fw_parse(blob, 60, &data, &len, &ioff) && data == blob + 20 && len == 40 && ioff == 3);
    assert(!dchid_fw_parse(blob, 59, &data, &len, &ioff));
    h[4] = 40; memcpy(blob, h, sizeof(h));
    assert(!dchid_fw_parse(blob, 60, &data, &len, &ioff));
    u8 cmd[9];
    assert(dchid_power_cmd(cmd, 2, 4, DCHID_POWER_ON, true) == 9 && cmd[0] == 0x40 && cmd[1] == 2 &&
           cmd[2] == 4 && cmd[3] == 2 && cmd[4] == 1 && cmd[8] == 0);
    assert(dchid_power_cmd(cmd, 1, 4, DCHID_POWER_OFF, false) == 4 && cmd[1] == 1);
}

int main(void)
{
    test_build_matches_uboot_enable();
    test_build_odd_length_pads();
    test_parse_init_keyboard_with_descriptor_and_gpio();
    test_fw_and_power();
    puts("dchid_proto: all tests passed");
    return 0;
}
