/* SPDX-License-Identifier: MIT */
/*
 * Apple MTP DockChannel HID protocol (keyboard + trackpad on M2/M3 laptops).
 *
 * Portable, allocation-free and OS-free: the Windows driver (and the host tests)
 * feed it bytes read from the DockChannel RX FIFO and get back decoded events;
 * it encodes the command packets that go into the TX FIFO.
 *
 * Wire format and flow follow the Asahi Linux driver
 * drivers/hid/dockchannel-hid/dockchannel-hid.c (GPL-2.0 OR MIT, used here
 * under MIT). Everything is little-endian and encoded byte by byte, so there
 * are no packed structs and no alignment assumptions.
 *
 *   packet  := hdr(8) body(hdr.length) checksum(4)
 *   hdr     := hdr_len=8 | channel | length:le16 | seq | iface | pad:le16
 *   body    := sub(8) payload(padded to 4)
 *   sub     := flags | unk | length:le16 | retcode:le32
 *   flags   := report type (bits 7:6) | request (bits 5:0)
 *   checksum: the le32 sum of every word of hdr + body + checksum == 0xffffffff
 *
 * Interface 0 ("comm") carries events from the MTP: INIT (one per HID
 * interface, with its HID report descriptor and optional GPIO request),
 * READY and GPIO commands. Other interfaces carry HID input reports.
 */
#ifndef APPLE_DCHID_H
#define APPLE_DCHID_H

#ifdef _KERNEL_MODE
#include <ntddk.h>
typedef UCHAR uint8_t;
typedef USHORT uint16_t;
typedef ULONG uint32_t;
typedef ULONGLONG uint64_t;
#ifndef __cplusplus
typedef unsigned char bool;
#define true 1
#define false 0
#endif
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#endif

#define DCHID_HDR_SIZE        8
#define DCHID_SUBHDR_SIZE     8
#define DCHID_CHECKSUM_SIZE   4
#define DCHID_MAX_INTERFACES  16
#define DCHID_NAME_SIZE       16
#define DCHID_GPIO_NAME_SIZE  32
/* Largest body we accept. Linux allows 0xffff; HID descriptors and touch
 * frames are far smaller, so keep the reassembly buffer modest. */
#define DCHID_MAX_BODY        4096
#define DCHID_MAX_PACKET      (DCHID_HDR_SIZE + DCHID_MAX_BODY + DCHID_CHECKSUM_SIZE)
#define DCHID_MAX_DESCRIPTOR  1024

#define DCHID_CHANNEL_CMD     0x11 /* acks to our commands */
#define DCHID_CHANNEL_REPORT  0x12 /* events and input reports */

#define DCHID_IFACE_COMM      0

/* HID report types as used in flags bits 7:6 (same values as Linux). */
#define DCHID_TYPE_INPUT      0
#define DCHID_TYPE_OUTPUT     1
#define DCHID_TYPE_FEATURE    2
#define DCHID_REQ_SET_REPORT  0
#define DCHID_REQ_GET_REPORT  1
#define DCHID_FLAGS(type, req) ((uint8_t)((((type) & 3u) << 6) | ((req) & 0x3fu)))

/* Comm events (first payload byte). */
#define DCHID_EVENT_GPIO_CMD  0xa0
#define DCHID_EVENT_INIT      0xf0
#define DCHID_EVENT_READY     0xf1

/* INIT block types. */
#define DCHID_INIT_HID_DESCRIPTOR 0
#define DCHID_INIT_GPIO_REQUEST   1
#define DCHID_INIT_TERMINATOR     2
#define DCHID_INIT_PRODUCT_NAME   7

/* Comm commands (first byte of a feature SET_REPORT to iface 0). */
#define DCHID_CMD_RESET_INTERFACE  0x40
#define DCHID_CMD_SEND_FIRMWARE    0x95
#define DCHID_CMD_ENABLE_INTERFACE 0xb4
#define DCHID_CMD_ACK_GPIO_CMD     0xa1

/* STM feature reports. */
#define DCHID_STM_REPORT_ID       0x10
#define DCHID_STM_REPORT_SERIAL   0x11

/* Multi-touch firmware container ("HIDF"), as extracted from macOS by Asahi. */
#define DCHID_FW_MAGIC 0x46444948u
#define DCHID_FW_VER   1u

enum dchid_status {
    DCHID_OK = 0,
    DCHID_NEED_MORE,      /* stream: not a whole packet yet */
    DCHID_E_ARG,
    DCHID_E_SPACE,        /* output buffer too small */
    DCHID_E_HEADER,       /* bad hdr_len or length */
    DCHID_E_CHECKSUM,
    DCHID_E_IFACE,        /* index out of range or unknown interface */
    DCHID_E_SUBHDR,
    DCHID_E_BUSY,         /* a command is already outstanding on that iface */
    DCHID_E_UNEXPECTED,   /* ack does not match the outstanding command */
    DCHID_E_FIRMWARE,
};

/* A validated packet; points into the caller's / reassembler's buffer. */
struct dchid_packet {
    uint8_t channel;
    uint8_t seq;
    uint8_t iface;
    uint8_t flags;
    uint32_t retcode;
    const uint8_t *payload;
    uint16_t payload_len;
};

/* Byte-stream reassembler for the DockChannel RX FIFO. */
struct dchid_rx_stream {
    uint8_t buf[DCHID_MAX_PACKET];
    size_t fill;
    uint32_t dropped; /* bytes discarded while resynchronising */
};

struct dchid_iface {
    bool present;
    char name[DCHID_NAME_SIZE + 1];
    uint8_t hid_desc[DCHID_MAX_DESCRIPTOR];
    uint16_t hid_desc_len;
    uint16_t gpio_id;                       /* 0: none */
    char gpio_name[DCHID_GPIO_NAME_SIZE + 1];
    bool init_complete;                     /* INIT with more_packets == 0 seen */
    bool deferred;                          /* waiting for STM before enabling */
    bool enable_sent;
    bool ready;
    /* outstanding command */
    uint8_t tx_seq;
    int out_report;                         /* -1: none */
    uint8_t out_flags;
};

enum dchid_event_kind {
    DCHID_EV_NONE = 0,
    DCHID_EV_ENABLE_IFACE,   /* host must send dchid_encode_enable(iface) */
    DCHID_EV_IFACE_READY,    /* iface may now receive reports / firmware */
    DCHID_EV_STM_READY,      /* host should GET_REPORT STM id + serial */
    DCHID_EV_GPIO_PULSE,     /* retcode 0: pulse gpio ~10 ms, then ack with 0.
                              * retcode != 0: unsupported, just ack with retcode. */
    DCHID_EV_CMD_DONE,       /* outstanding command on iface completed */
    DCHID_EV_INPUT_REPORT,   /* HID input report for iface */
};

struct dchid_event {
    enum dchid_event_kind kind;
    uint8_t iface;
    uint16_t gpio_id;        /* GPIO_PULSE */
    uint8_t gpio_cmd[8];     /* GPIO_PULSE: raw command, echoed in the ack */
    uint8_t gpio_cmd_len;
    uint32_t retcode;        /* CMD_DONE, GPIO_PULSE */
    const uint8_t *data;     /* CMD_DONE (response, report id stripped) / INPUT_REPORT */
    uint16_t len;
};

struct dchid_session {
    struct dchid_iface ifaces[DCHID_MAX_INTERFACES];
    bool stm_ready;
};

/* --- framing --------------------------------------------------------------- */
uint32_t dchid_checksum(const uint8_t *p, size_t len);  /* len multiple of 4 */
enum dchid_status dchid_encode(uint8_t iface, uint8_t seq, uint8_t flags,
                               const uint8_t *msg, size_t msg_len,
                               uint8_t *out, size_t cap, size_t *out_len);
enum dchid_status dchid_decode(const uint8_t *pkt, size_t len, struct dchid_packet *out);

void dchid_rx_reset(struct dchid_rx_stream *s);
/* Append FIFO bytes; returns how many were consumed (stops after a full packet). */
size_t dchid_rx_push(struct dchid_rx_stream *s, const uint8_t *data, size_t len);
/* Returns DCHID_OK and a packet view (valid until the next push/pop), or NEED_MORE. */
enum dchid_status dchid_rx_peek(struct dchid_rx_stream *s, struct dchid_packet *out);
void dchid_rx_pop(struct dchid_rx_stream *s);

/* --- comm command payloads ------------------------------------------------- */
size_t dchid_payload_enable(uint8_t iface, uint8_t out[2]);
size_t dchid_payload_reset(uint8_t iface, uint8_t state, uint8_t out[4]);
size_t dchid_payload_firmware(uint8_t iface, uint64_t dma_addr, uint32_t size, uint8_t out[16]);
size_t dchid_payload_gpio_ack(uint32_t retcode, const uint8_t *cmd, size_t cmd_len,
                              uint8_t *out, size_t cap);

/* --- session ----------------------------------------------------------------- */
void dchid_session_init(struct dchid_session *s);
/* Start a command on iface and encode its packet. Only one may be outstanding
 * per interface; the ack arrives as DCHID_EV_CMD_DONE. */
enum dchid_status dchid_session_command(struct dchid_session *s, uint8_t iface,
                                        uint8_t type, uint8_t req,
                                        const uint8_t *msg, size_t msg_len,
                                        uint8_t *out, size_t cap, size_t *out_len);
/* Abandon the outstanding command on iface (e.g. after a host-side timeout).
 * Like Linux, the sequence number still advances. */
void dchid_session_cancel(struct dchid_session *s, uint8_t iface);
/* Feed one decoded packet; writes up to `max` events. */
enum dchid_status dchid_session_handle(struct dchid_session *s, const struct dchid_packet *p,
                                       struct dchid_event *ev, size_t max, size_t *n_ev);
const struct dchid_iface *dchid_session_find(const struct dchid_session *s, const char *name);

/* --- multi-touch firmware container --------------------------------------------- */
/* Validates the HIDF header; returns the data slice and patches the iface byte
 * into `patched` (caller buffer of at least *data_len bytes) when needed. */
enum dchid_status dchid_firmware_parse(const uint8_t *blob, size_t blob_len, uint8_t iface,
                                       uint8_t *patched, size_t cap, size_t *data_len);

#endif
