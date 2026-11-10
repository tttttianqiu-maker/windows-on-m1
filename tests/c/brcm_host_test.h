/* SPDX-License-Identifier: MIT */
/*
 * Host build of m1n1's Broadcom FullMAC protocol code (brcm_msgbuf.c, brcm_fwil.c) for
 * tests/c/test_brcm_msgbuf.c: the few m1n1 interfaces those files use, implemented by the
 * test (MMIO, TCM, DART, clock) against a simulated dongle.
 */
#ifndef BRCM_HOST_TEST_H
#define BRCM_HOST_TEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

#define PACKED        __attribute__((packed))
#define BIT(x)        (1UL << (x))
#define SZ_16K        (1 << 14)
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#define ARRAY_SIZE(s) (sizeof(s) / sizeof((s)[0]))
#define min(a, b)     (((a) < (b)) ? (a) : (b))
#define max(a, b)     (((a) > (b)) ? (a) : (b))
#define sysop(op)     do { } while (0)
#define dma_wmb()     do { } while (0)
#define dma_rmb()     do { } while (0)

/* clock: microsecond ticks, advanced by udelay/mdelay (which also run the fake dongle) */
u64 get_ticks(void);
u64 ticks_to_msecs(u64 ticks);
u64 ticks_to_usecs(u64 ticks);
void udelay(u32 us);
void mdelay(u32 ms);

u32 read32(u64 addr);
void write32(u64 addr, u32 val);
void dc_civac_range(void *addr, size_t length);
void *host_memalign(size_t align, size_t size);
#define memalign host_memalign

/* ADT / DART (dma_init) */
extern void *adt;
int adt_path_offset(const void *adt, const char *path);
int adt_path_offset_trace(const void *adt, const char *path, int *offsets);
int adt_get_reg(const void *adt, int *path, const char *prop, int idx, u64 *addr, u64 *size);
typedef struct host_dart dart_dev_t;
enum dart_type_t { DART_T8020, DART_T6000, DART_T8110 };
dart_dev_t *dart_init_adt(const char *path, int instance, int device, bool keep_pts);
dart_dev_t *dart_init(uintptr_t base, u8 device, bool keep_pts, enum dart_type_t type);
int dart_map(dart_dev_t *dart, uintptr_t iova, void *bfr, size_t len);

#endif
