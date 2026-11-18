/*
 * OunceMTP - MTP responder (portable C, no libnx dependency).
 * SPDX-License-Identifier: ISC
 */
#ifndef OUNCEMTP_MTP_H
#define OUNCEMTP_MTP_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    const char *root_path;      /* filesystem root exposed as the storage */
    const char *storage_desc;   /* "SD Card"                                */
    const char *manufacturer;
    const char *model;
    const char *friendly_name;  /* shown as the device name in Explorer     */
    const char *version;
    const char *serial;
    volatile int *stop;         /* worker exits when *stop != 0              */
} mtp_config_t;

/* Allocates transfer buffers. Returns false on OOM. */
bool mtp_init(const mtp_config_t *cfg);
/* Serves until *cfg->stop is set or the transport fails fatally. */
void mtp_run(void);
void mtp_exit(void);

#endif
