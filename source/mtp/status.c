/*
 * OunceMTP - shared status between the MTP worker thread and the UI.
 * SPDX-License-Identifier: ISC
 *
 * The worker writes, the UI thread snapshots once per frame. A single mutex
 * is plenty: progress updates happen once per 1 MiB chunk, not per byte.
 */
#include "status.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef __SWITCH__
#include <switch.h>
static Mutex g_lock;
#define LOCK()   mutexLock(&g_lock)
#define UNLOCK() mutexUnlock(&g_lock)
uint64_t status_now_ms(void) { return armTicksToNs(armGetSystemTick()) / 1000000ull; }
#else
#include <pthread.h>
#include <time.h>
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK()   pthread_mutex_lock(&g_lock)
#define UNLOCK() pthread_mutex_unlock(&g_lock)
uint64_t status_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
#endif

static app_status_t g_st;

void status_init(void)
{
#ifdef __SWITCH__
    mutexInit(&g_lock);
#endif
    memset(&g_st, 0, sizeof(g_st));
}

void status_set_state(app_state_t s)
{
    LOCK();
    /* A transfer in progress is a sub-state of an open session. */
    g_st.state = s;
    UNLOCK();
}

void status_set_error(const char *fmt, ...)
{
    va_list ap;
    LOCK();
    va_start(ap, fmt);
    vsnprintf(g_st.error, sizeof(g_st.error), fmt, ap);
    va_end(ap);
    g_st.state = ST_ERROR;
    UNLOCK();
}

void status_set_speed(uint32_t mps)
{
    LOCK();
    g_st.usb_speed_mps = mps;
    UNLOCK();
}

void status_log(const char *fmt, ...)
{
    va_list ap;
    LOCK();
    char *line = g_st.log[g_st.log_head % STATUS_LOG_LINES];
    va_start(ap, fmt);
    vsnprintf(line, STATUS_LINE_LEN, fmt, ap);
    va_end(ap);
    g_st.log_head++;
    UNLOCK();
}

void status_xfer_begin(xfer_dir_t dir, const char *name, uint64_t total)
{
    LOCK();
    g_st.state = ST_TRANSFER;
    g_st.dir = dir;
    snprintf(g_st.name, sizeof(g_st.name), "%s", name);
    g_st.done = 0;
    g_st.total = total;
    g_st.start_ms = status_now_ms();
    UNLOCK();
}

void status_xfer_progress(uint64_t done)
{
    LOCK();
    g_st.done = done;
    UNLOCK();
}

void status_xfer_end(bool ok)
{
    LOCK();
    if (ok) {
        if (g_st.dir == DIR_TO_PC)      { g_st.bytes_to_pc += g_st.done;     g_st.files_to_pc++; }
        if (g_st.dir == DIR_TO_SWITCH)  { g_st.bytes_to_switch += g_st.done; g_st.files_to_switch++; }
    }
    g_st.dir = DIR_NONE;
    if (g_st.state == ST_TRANSFER) g_st.state = ST_SESSION;
    UNLOCK();
}

void status_snapshot(app_status_t *out)
{
    LOCK();
    *out = g_st;
    UNLOCK();
}
