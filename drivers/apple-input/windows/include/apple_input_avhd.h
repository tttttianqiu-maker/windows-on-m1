#pragma once
/*
 * Guest view of the m1n1 "AVHD" virtual HID device (J613 MacBook Air M3).
 * Must match m1n1_windows/src/hv_dchid.h. All accesses are 32-bit.
 */
#include "apple_input_device.h"

#define AI_AVHD_IPA          0x5300000000ll
#define AI_AVHD_SIZE         0x1000u
#define AI_AVHD_MAGIC_VALUE  0x44485641u /* 'AVHD' */

#define AI_AVHD_MAGIC     0x000u
#define AI_AVHD_VERSION   0x004u
#define AI_AVHD_STATUS    0x008u
#define AI_AVHD_IRQ_ACK   0x00cu
#define AI_AVHD_DESC_SEL  0x010u
#define AI_AVHD_DESC_LEN  0x014u
#define AI_AVHD_RX_LEN    0x020u
#define AI_AVHD_RX_IFACE  0x024u
#define AI_AVHD_RX_POP    0x028u
#define AI_AVHD_RX_DROPS  0x02cu
#define AI_AVHD_IDS       0x030u
#define AI_AVHD_DIMS      0x040u
#define AI_AVHD_DESC      0x400u
#define AI_AVHD_REPORT    0x800u

#define AI_AVHD_DESC_WINDOW   0x400u
#define AI_AVHD_REPORT_WINDOW 0x800u

#define AI_AVHD_STATUS_KEYBOARD  0x1u
#define AI_AVHD_STATUS_TRACKPAD  0x2u
#define AI_AVHD_STATUS_MTP_UP    0x4u
#define AI_AVHD_STATUS_DIMS      0x8u

#define AI_AVHD_IFACE_KEYBOARD 0u
#define AI_AVHD_IFACE_TRACKPAD 1u

static __forceinline ULONG AiAvhdRead(PAI_DEVICE_CONTEXT Context, ULONG Offset)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(Context->AvhdRegisters + Offset));
}

static __forceinline VOID AiAvhdWrite(PAI_DEVICE_CONTEXT Context, ULONG Offset,
                                      ULONG Value)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(Context->AvhdRegisters + Offset), Value);
}

/* Copy Length bytes from an AVHD window using aligned 32-bit reads. */
#ifndef _Out_writes_bytes_
#define _Out_writes_bytes_(n)
#endif
static __inline VOID AiAvhdReadWindow(PAI_DEVICE_CONTEXT Context, ULONG Offset,
                                      _Out_writes_bytes_(Length) PUCHAR Out,
                                      ULONG Length)
{
    for (ULONG i = 0; i < Length; i += 4u) {
        ULONG word = AiAvhdRead(Context, Offset + i);
        for (ULONG b = 0; b < 4u && i + b < Length; b++)
            Out[i + b] = (UCHAR)(word >> (8u * b));
    }
}
