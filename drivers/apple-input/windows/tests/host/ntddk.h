/* Host-only stand-ins for WDK headers, just enough for tests/avhd_sim_test.c. */
#pragma once
#include "guiddef.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int32_t NTSTATUS; typedef uint8_t UCHAR,*PUCHAR,BOOLEAN; typedef uint16_t USHORT,*PUSHORT; typedef uint32_t ULONG,*PULONG;
typedef int32_t LONG; typedef int64_t LONG64,LONGLONG; typedef uint64_t ULONGLONG; typedef size_t SIZE_T; typedef void VOID,*PVOID;
typedef char CHAR; typedef const wchar_t *PCWSTR; typedef uint8_t KIRQL; typedef uintptr_t KSPIN_LOCK;
typedef union { struct { ULONG LowPart; LONG HighPart; }; LONGLONG QuadPart; } LARGE_INTEGER, PHYSICAL_ADDRESS;
typedef struct { USHORT Length, MaximumLength; wchar_t *Buffer; } UNICODE_STRING,*PUNICODE_STRING;
typedef struct _DRIVER_OBJECT *PDRIVER_OBJECT; typedef struct _DEVICE_OBJECT *PDEVICE_OBJECT;
typedef struct { UCHAR Type; union { struct { PHYSICAL_ADDRESS Start; ULONG Length; } Memory; struct { ULONG Level, Vector; uintptr_t Affinity; } Interrupt; } u; } CM_PARTIAL_RESOURCE_DESCRIPTOR,*PCM_PARTIAL_RESOURCE_DESCRIPTOR;

#define TRUE 1
#define FALSE 0
#define CmResourceTypeMemory 3
#define CmResourceTypeInterrupt 2
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000D)
#define STATUS_DEVICE_CONFIGURATION_ERROR ((NTSTATUS)0xC0000182)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xC000009A)
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xC00000A3)
#define STATUS_DEVICE_BUSY ((NTSTATUS)0x80000011)
#define STATUS_INVALID_BUFFER_SIZE ((NTSTATUS)0xC0000206)
#define STATUS_DEVICE_PROTOCOL_ERROR ((NTSTATUS)0xC0000186)
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xC00000BB)
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001)
#define STATUS_INVALID_DEVICE_STATE ((NTSTATUS)0xC0000184)
#define STATUS_DATA_ERROR ((NTSTATUS)0xC000003E)
#define STATUS_IO_TIMEOUT ((NTSTATUS)0xC00000B5)
#define STATUS_INTERNAL_ERROR ((NTSTATUS)0xC00000E5)
#define STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xC0000023)
#define STATUS_INVALID_DEVICE_REQUEST ((NTSTATUS)0xC0000010)
#define NT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#define UNREFERENCED_PARAMETER(x) (void)(x)
#define PAGED_CODE() ((void)0)
#define MAXUCHAR 0xff
#define C_ASSERT(e) _Static_assert(e, #e)
#define FIELD_OFFSET(t,f) offsetof(t,f)
#define RTL_NUMBER_OF(a) (sizeof(a)/sizeof((a)[0]))
#define PAGE_READWRITE 4
#define PAGE_NOCACHE 0x200
#define KEY_READ 0x20019
#define __forceinline inline __attribute__((always_inline))
#define __inline inline
void RtlZeroMemory(void*,size_t); void RtlCopyMemory(void*,const void*,size_t); void RtlInitUnicodeString(PUNICODE_STRING,PCWSTR);
LONG64 InterlockedIncrement64(volatile LONG64*); LARGE_INTEGER KeQueryPerformanceCounter(LARGE_INTEGER*);
void KeInitializeSpinLock(KSPIN_LOCK*); void KeAcquireSpinLock(KSPIN_LOCK*,KIRQL*); void KeReleaseSpinLock(KSPIN_LOCK*,KIRQL);
PVOID MmMapIoSpaceEx(PHYSICAL_ADDRESS,SIZE_T,ULONG); void MmUnmapIoSpace(PVOID,SIZE_T);
ULONG READ_REGISTER_ULONG(volatile ULONG*); void WRITE_REGISTER_ULONG(volatile ULONG*,ULONG);
typedef NTSTATUS DRIVER_INITIALIZE(PDRIVER_OBJECT,PUNICODE_STRING);
#define MAXUSHORT 0xffff
#define MAXULONGLONG (~(ULONGLONG)0)
#define AI_KERNEL_FIXED_WIDTH_TYPES
#define AI_KERNEL_SIGNED_FIXED_WIDTH_TYPES
#define AI_KERNEL_BOOL_TYPE
