/* J613 MTP adapter: J313 fixtures with the SPI prefix removed must decode identically. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "apple_mtp.h"
#include "fixtures/j313_trackpad_sanitized.h"

static void roundtrip(const uint8_t *spi, size_t len)
{
    struct ai_apple_trackpad_frame want, got;
    uint8_t mtp[512], rebuilt[512];
    size_t n = 0;

    assert(ai_apple_trackpad_decode(spi, len, &want) == AI_OK);
    /* MTP report = SPI report minus the 8-byte mouse prefix; its first byte is 0x75. */
    memcpy(mtp, spi + 8, len - 8);
    mtp[0] = AI_MTP_TRACKPAD_REPORT_ID;
    assert(ai_mtp_trackpad_to_spi(mtp, len - 8, rebuilt, sizeof(rebuilt), &n) == AI_OK);
    assert(n == len);
    assert(ai_apple_trackpad_decode(rebuilt, n, &got) == AI_OK);
    assert(got.button == want.button && got.count == want.count);
    for (uint8_t i = 0; i < got.count; i++)
        assert(got.contacts[i].x == want.contacts[i].x && got.contacts[i].y == want.contacts[i].y);
}

int main(void)
{
    roundtrip(j313_trackpad_one_contact_x, sizeof(j313_trackpad_one_contact_x));

    uint8_t bad[64] = {0x02};
    uint8_t out[128];
    size_t n;
    assert(ai_mtp_trackpad_to_spi(bad, 40, out, sizeof(out), &n) == AI_ERR_PROTOCOL);
    bad[0] = 0x75;
    assert(ai_mtp_trackpad_to_spi(bad, 37, out, sizeof(out), &n) == AI_ERR_LENGTH);
    assert(ai_mtp_trackpad_to_spi(bad, 40, out, 45, &n) == AI_ERR_LENGTH);
    assert(ai_mtp_trackpad_to_spi(bad, 38, out, sizeof(out), &n) == AI_OK && n == 46 &&
           out[0] == 0x02 && out[8] == 0x75);

    /* dimensions: 2.00 cm x 1.00 cm, x -100..100, y -50..50 */
    uint8_t dims[16] = {0xd0, 0x07, 0, 0, 0xe8, 0x03, 0, 0, 0x9c, 0xff, 0xce, 0xff, 0x64, 0, 0x32, 0};
    struct ai_trackpad_dimensions d;
    assert(ai_mtp_dimensions_parse(dims, &d) == AI_OK && d.valid);
    assert(d.width_hundredths_mm == 2000 && d.height_hundredths_mm == 1000);
    assert(d.min_x == -100 && d.max_x == 100 && d.min_y == -50 && d.max_y == 50);
    dims[12] = 0x9c; dims[13] = 0xff;   /* max_x == min_x */
    assert(ai_mtp_dimensions_parse(dims, &d) != AI_OK);
    puts("apple_mtp: all tests passed");
    return 0;
}
