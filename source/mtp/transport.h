/*
 * OunceMTP - USB file transfer for Switch 2 homebrew
 * SPDX-License-Identifier: ISC
 *
 * Transport interface between the MTP responder (mtp.c, portable) and the
 * USB device stack. On the console this is usb_transport.c (usb:ds); the
 * host tests implement it over a socket that emulates USB bulk semantics.
 *
 * Semantics are those of a USB bulk endpoint pair:
 *  - A read completes when the posted size is filled or the host ends its
 *    transfer with a short packet (possibly zero-length).
 *  - A write is one device->host transfer. The host only sees the end of a
 *    transfer at a short packet, so the caller must send a zero-length
 *    packet when a data phase is an exact multiple of tp_max_packet().
 *  - At most one read and one write may be outstanding at a time.
 *  - Buffers passed to begin calls must be 4 KiB aligned and stay valid
 *    until the matching end call returns.
 */
#ifndef OUNCEMTP_TRANSPORT_H
#define OUNCEMTP_TRANSPORT_H

#include <stdint.h>

enum {
    TP_OK           = 0,
    TP_TIMEOUT      = -1,  /* stall timeout hit, transfer was cancelled  */
    TP_DISCONNECTED = -2,  /* cable pulled / host deconfigured us        */
    TP_STOPPED      = -3,  /* the app asked the worker to stop           */
    TP_ERROR        = -4,  /* service error                              */
};

/* 0 = wait forever (still returns early on stop/disconnect). */
#define TP_NO_TIMEOUT 0u

int      tp_init(void);
void     tp_exit(void);

/* TP_OK once the host has configured the device, TP_TIMEOUT otherwise. */
int      tp_wait_configured(uint32_t timeout_ms);
int      tp_is_configured(void);

/* Bulk max packet size of the current connection (512 HS, 1024 SS). */
uint32_t tp_max_packet(void);

int      tp_read_begin(void *buf, uint32_t size);
int      tp_read_end(uint32_t *transferred, uint32_t timeout_ms);
int      tp_write_begin(const void *buf, uint32_t size);
int      tp_write_end(uint32_t *transferred, uint32_t timeout_ms);

/* Cancel anything in flight (used after errors to resynchronise). */
void     tp_abort(void);

#ifdef __SWITCH__
/* Console-only extras (usb_transport.c). */
void     tp_set_stop_flag(volatile int *flag);
void     tp_set_serial(const char *serial);   /* call before tp_init */
uint32_t tp_last_result(void);                /* libnx Result of the last failure */
const char *tp_last_step(void);               /* setup step that failed ("" if none) */
#endif

/* Convenience synchronous wrappers. */
static inline int tp_read(void *buf, uint32_t size, uint32_t *n, uint32_t timeout_ms)
{
    int r = tp_read_begin(buf, size);
    return r ? r : tp_read_end(n, timeout_ms);
}

static inline int tp_write(const void *buf, uint32_t size, uint32_t timeout_ms)
{
    uint32_t n;
    int r = tp_write_begin(buf, size);
    return r ? r : tp_write_end(&n, timeout_ms);
}

#endif
