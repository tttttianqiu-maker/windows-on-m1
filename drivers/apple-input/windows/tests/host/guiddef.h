/* Host-only stand-ins for WDK headers, just enough for tests/avhd_sim_test.c. */
#pragma once
#include <stdint.h>
typedef struct { uint32_t a; uint16_t b,c; uint8_t d[8]; } GUID;
#define DEFINE_GUID(n,l,w1,w2,b1,b2,b3,b4,b5,b6,b7,b8) extern const GUID n
#define CTL_CODE(d,f,m,a) (((d)<<16)|((a)<<14)|((f)<<2)|(m))
#define METHOD_BUFFERED 0
#define FILE_READ_ACCESS 1
#define FILE_ANY_ACCESS 0
#define FILE_DEVICE_UNKNOWN 0x22
