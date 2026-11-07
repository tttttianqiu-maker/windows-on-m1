#include "apple_mtp.h"

enum ai_status ai_mtp_trackpad_to_spi(const uint8_t *mtp, size_t length,
                                      uint8_t *out, size_t capacity,
                                      size_t *out_length)
{
    if (!mtp || !out || !out_length)
        return AI_ERR_ARGUMENT;
    *out_length = 0;
    if (length < AI_MTP_HEADER_SIZE)
        return AI_ERR_LENGTH;
    if (mtp[0] != AI_MTP_TRACKPAD_REPORT_ID)
        return AI_ERR_PROTOCOL;
    if (length > capacity || capacity - length < AI_MTP_SPI_PREFIX_SIZE)
        return AI_ERR_LENGTH;

    AI_MEMSET(out, AI_MTP_SPI_PREFIX_SIZE);
    out[0] = AI_MTP_SPI_REPORT_ID;
    out[1] = mtp[AI_MTP_BUTTON_OFFSET];   /* decoder checks it equals offset 31 */
    AI_MEMCPY(out + AI_MTP_SPI_PREFIX_SIZE, mtp, length);
    *out_length = length + AI_MTP_SPI_PREFIX_SIZE;
    return AI_OK;
}

enum ai_status ai_mtp_dimensions_parse(const uint8_t dims[AI_MTP_DIMENSIONS_SIZE],
                                       struct ai_trackpad_dimensions *out)
{
    uint8_t report[1u + AI_MTP_DIMENSIONS_SIZE];

    if (!dims || !out)
        return AI_ERR_ARGUMENT;
    report[0] = AI_MTP_DIMENSIONS_REPORT_ID;
    AI_MEMCPY(report + 1, dims, AI_MTP_DIMENSIONS_SIZE);
    return ai_trackpad_dimensions_parse(report, sizeof(report), out);
}
