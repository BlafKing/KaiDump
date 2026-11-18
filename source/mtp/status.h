/*
 * OunceMTP - shared status between the MTP worker thread and the UI.
 * SPDX-License-Identifier: ISC
 */
#ifndef OUNCEMTP_STATUS_H
#define OUNCEMTP_STATUS_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    ST_STARTING = 0,
    ST_WAITING_CABLE,   /* usb:ds up, host has not configured us     */
    ST_CONNECTED,       /* configured, no MTP session yet            */
    ST_SESSION,         /* MTP session open, idle                    */
    ST_TRANSFER,        /* moving a file                             */
    ST_ERROR,           /* fatal (e.g. usb:ds unavailable)           */
} app_state_t;

typedef enum { DIR_NONE = 0, DIR_TO_PC, DIR_TO_SWITCH, DIR_COPY } xfer_dir_t;

#define STATUS_LOG_LINES 14   /* fits the 45-row console with room to spare */
#define STATUS_LINE_LEN  96

typedef struct {
    app_state_t state;
    char        error[2 * STATUS_LINE_LEN];  /* shown over two lines */
    uint32_t    usb_speed_mps;          /* 512 / 1024, 0 when detached */

    /* current transfer */
    xfer_dir_t  dir;
    char        name[STATUS_LINE_LEN];
    uint64_t    done, total;            /* total == UINT64_MAX: unknown */
    uint64_t    start_ms;

    /* session totals */
    uint64_t    bytes_to_pc, bytes_to_switch;
    uint32_t    files_to_pc, files_to_switch;

    /* recent events, ring buffer */
    char        log[STATUS_LOG_LINES][STATUS_LINE_LEN];
    uint32_t    log_head;               /* number of lines ever written */
} app_status_t;

void     status_init(void);
uint64_t status_now_ms(void);

void status_set_state(app_state_t s);
void status_set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void status_set_speed(uint32_t mps);
void status_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void status_xfer_begin(xfer_dir_t dir, const char *name, uint64_t total);
void status_xfer_progress(uint64_t done);
void status_xfer_end(bool ok);

/* Copy a consistent snapshot for rendering. */
void status_snapshot(app_status_t *out);

#endif
