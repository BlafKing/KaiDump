/*
 * OunceMTP - little-endian MTP dataset builder / parser and UTF-8<->UTF-16.
 * SPDX-License-Identifier: ISC
 */
#ifndef OUNCEMTP_DATASET_H
#define OUNCEMTP_DATASET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* MTP strings: u8 char count (incl. NUL, max 255) + UTF-16LE code units. */
#define MTP_STR_MAX_UNITS 254

/* Returns number of UTF-16 units written (no terminator), max `max_units`. */
size_t utf8_to_utf16(const char *in, uint16_t *out, size_t max_units);
/* Always NUL terminates `out`. Returns bytes written (no terminator). */
size_t utf16_to_utf8(const uint16_t *in, size_t units, char *out, size_t out_size);

/* ---- builder: growable buffer ------------------------------------------ */
typedef struct {
    uint8_t *data;
    size_t   len, cap;
    bool     oom;
} dbuf_t;

void dbuf_init(dbuf_t *b);
void dbuf_free(dbuf_t *b);
void dbuf_reset(dbuf_t *b);
void dbuf_put(dbuf_t *b, const void *p, size_t n);
void dbuf_u8(dbuf_t *b, uint8_t v);
void dbuf_u16(dbuf_t *b, uint16_t v);
void dbuf_u32(dbuf_t *b, uint32_t v);
void dbuf_u64(dbuf_t *b, uint64_t v);
void dbuf_u128(dbuf_t *b, uint64_t lo, uint64_t hi);
void dbuf_str(dbuf_t *b, const char *utf8);         /* NULL/"" -> 0x00 */
void dbuf_arr16(dbuf_t *b, const uint16_t *a, uint32_t n);
void dbuf_arr32(dbuf_t *b, const uint32_t *a, uint32_t n);
/* Patch a u32 at an earlier offset (array counts written after the fact). */
void dbuf_patch_u32(dbuf_t *b, size_t off, uint32_t v);

/* ---- parser ------------------------------------------------------------ */
typedef struct {
    const uint8_t *p;
    size_t len, pos;
    bool   err;    /* set on any over-read; values read after that are 0 */
} dparse_t;

void     dparse_init(dparse_t *d, const void *p, size_t len);
uint8_t  dparse_u8(dparse_t *d);
uint16_t dparse_u16(dparse_t *d);
uint32_t dparse_u32(dparse_t *d);
uint64_t dparse_u64(dparse_t *d);
void     dparse_skip(dparse_t *d, size_t n);
/* Reads an MTP string into UTF-8. */
void     dparse_str(dparse_t *d, char *out, size_t out_size);

/* Unaligned LE helpers for container headers. */
static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

#endif
