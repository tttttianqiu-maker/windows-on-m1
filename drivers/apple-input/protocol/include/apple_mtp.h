#pragma once
/*
 * J613 (M3) MTP trackpad and keyboard reports, as delivered by the m1n1 AVHD
 * device (m1n1_windows/src/hv_dchid.h).
 *
 * The MTP trackpad report (HID input report 0x75) is the J313 SPI trackpad
 * report without its 8-byte SPI mouse prefix: Linux hid-magicmouse.c strips
 * exactly that prefix from SPI reports and hands the rest to the same MTP
 * parser (magicmouse_raw_event_spi -> magicmouse_raw_event_mtp). Re-adding the
 * prefix lets the validated J313 decoder (ai_apple_trackpad_decode) and the
 * Precision Touchpad encoder run unchanged.
 */
#include "apple_trackpad.h"

#define AI_MTP_TRACKPAD_REPORT_ID 0x75u
#define AI_MTP_SPI_PREFIX_SIZE 8u
#define AI_MTP_SPI_REPORT_ID 0x02u
#define AI_MTP_HEADER_SIZE 38u          /* struct tp_header in hid-magicmouse.c */
#define AI_MTP_BUTTON_OFFSET 23u
#define AI_MTP_DIMENSIONS_REPORT_ID 0xd9u
#define AI_MTP_DIMENSIONS_SIZE 16u

/* Rebuild the J313 SPI-framed trackpad report from an MTP 0x75 report. */
enum ai_status ai_mtp_trackpad_to_spi(const uint8_t *mtp, size_t length,
                                      uint8_t *out, size_t capacity,
                                      size_t *out_length);
/* Parse the 16 AVHD dimension bytes (feature report 0xd9 without its ID). */
enum ai_status ai_mtp_dimensions_parse(const uint8_t dims[AI_MTP_DIMENSIONS_SIZE],
                                       struct ai_trackpad_dimensions *out);
