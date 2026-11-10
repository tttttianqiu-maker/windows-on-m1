/* SPDX-License-Identifier: MIT */
/*
 * Host build of m1n1's virtio-net device model (m1n1_windows/src/hv_vnet.c) for
 * tests/c/test_vnet.c: the m1n1 interfaces it uses (stage-2 hooks, guest memory, vGIC
 * injection, CPU id, IPIs, brcm_tx), implemented by the test.
 */
#ifndef VNET_HOST_TEST_H
#define VNET_HOST_TEST_H

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
typedef int32_t s32;
typedef int64_t s64;

#define BIT(x)        (1UL << (x))
#define UNUSED(x)     (void)(x)
#define ARRAY_SIZE(s) (sizeof(s) / sizeof((s)[0]))
#define min(a, b)     (((a) < (b)) ? (a) : (b))

#define HV_IRQ_SYNTHETIC_VNET_VINTID 67u

struct exc_info;
typedef bool(hv_hook_t)(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width);
int hv_map_hook(u64 from, hv_hook_t *hook, u64 size);
int hv_unmap(u64 from, u64 size);
u64 hv_ipa_to_pa(u64 ipa);

extern int boot_cpu_idx;
int smp_id(void);
void smp_send_ipi(int cpu);

bool hv_vgic3_irq_enabled(u32 vintid);
int hv_vgic3_get_free_lr(void);
u8 hv_vgic3_get_priority(u32 vintid);
void hv_vgic3_inject_irq(u32 vintid, u8 priority, bool active, bool pending, bool hw_status,
                         u64 hw_irq);

int brcm_tx(const u8 *frame, u32 len);
u64 get_ticks(void);
u64 ticks_to_msecs(u64 ticks);

#endif
