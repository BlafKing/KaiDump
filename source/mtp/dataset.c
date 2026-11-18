/*
 * OunceMTP - little-endian MTP dataset builder / parser and UTF-8<->UTF-16.
 * SPDX-License-Identifier: ISC
 */
#include "dataset.h"

#include <stdlib.h>
#include <string.h>

/* ---- UTF conversion ------------------------------------------------------
 * File names on the SD card are UTF-8; MTP carries UTF-16LE. Invalid input
 * sequences become U+FFFD rather than aborting, so a single odd file name
 * can never break a whole directory listing. */

static uint32_t utf8_next(const unsigned char **ps)
{
    const unsigned char *s = *ps;
    uint32_t c = *s++;
    int extra;
    uint32_t min;

    if (c < 0x80)              { *ps = s; return c; }
    else if ((c & 0xE0) == 0xC0) { c &= 0x1F; extra = 1; min = 0x80; }
    else if ((c & 0xF0) == 0xE0) { c &= 0x0F; extra = 2; min = 0x800; }
    else if ((c & 0xF8) == 0xF0) { c &= 0x07; extra = 3; min = 0x10000; }
    else                       { *ps = s; return 0xFFFD; }

    for (int i = 0; i < extra; i++) {
        if ((*s & 0xC0) != 0x80) { *ps = s; return 0xFFFD; }  /* truncated */
        c = (c << 6) | (*s++ & 0x3F);
    }
    *ps = s;
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return 0xFFFD;
    return c;
}

size_t utf8_to_utf16(const char *in, uint16_t *out, size_t max_units)
{
    const unsigned char *s = (const unsigned char *)in;
    size_t n = 0;
    while (*s) {
        uint32_t c = utf8_next(&s);
        if (c >= 0x10000) {
            if (n + 2 > max_units) break;
            c -= 0x10000;
            out[n++] = (uint16_t)(0xD800 | (c >> 10));
            out[n++] = (uint16_t)(0xDC00 | (c & 0x3FF));
        } else {
            if (n + 1 > max_units) break;
            out[n++] = (uint16_t)c;
        }
    }
    return n;
}

size_t utf16_to_utf8(const uint16_t *in, size_t units, char *out, size_t out_size)
{
    size_t o = 0;
    if (out_size == 0) return 0;
    for (size_t i = 0; i < units; i++) {
        uint32_t c = in[i];
        if (c == 0) break;
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < units && in[i + 1] >= 0xDC00 && in[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (in[i + 1] - 0xDC00);
            i++;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;  /* lone surrogate */
        }
        char tmp[4];
        size_t k;
        if (c < 0x80)       { tmp[0] = (char)c; k = 1; }
        else if (c < 0x800) { tmp[0] = (char)(0xC0 | c >> 6); tmp[1] = (char)(0x80 | (c & 0x3F)); k = 2; }
        else if (c < 0x10000) {
            tmp[0] = (char)(0xE0 | c >> 12); tmp[1] = (char)(0x80 | ((c >> 6) & 0x3F));
            tmp[2] = (char)(0x80 | (c & 0x3F)); k = 3;
        } else {
            tmp[0] = (char)(0xF0 | c >> 18); tmp[1] = (char)(0x80 | ((c >> 12) & 0x3F));
            tmp[2] = (char)(0x80 | ((c >> 6) & 0x3F)); tmp[3] = (char)(0x80 | (c & 0x3F)); k = 4;
        }
        if (o + k + 1 > out_size) break;
        memcpy(out + o, tmp, k);
        o += k;
    }
    out[o] = 0;
    return o;
}

/* ---- builder ------------------------------------------------------------ */

void dbuf_init(dbuf_t *b) { memset(b, 0, sizeof(*b)); }
void dbuf_free(dbuf_t *b) { free(b->data); memset(b, 0, sizeof(*b)); }
void dbuf_reset(dbuf_t *b) { b->len = 0; b->oom = false; }

void dbuf_put(dbuf_t *b, const void *p, size_t n)
{
    if (b->oom) return;
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap : 256;
        while (nc < b->len + n) nc *= 2;
        uint8_t *nd = realloc(b->data, nc);
        if (!nd) { b->oom = true; return; }
        b->data = nd;
        b->cap = nc;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

void dbuf_u8(dbuf_t *b, uint8_t v) { dbuf_put(b, &v, 1); }
void dbuf_u16(dbuf_t *b, uint16_t v) { uint8_t t[2]; wr16(t, v); dbuf_put(b, t, 2); }
void dbuf_u32(dbuf_t *b, uint32_t v) { uint8_t t[4]; wr32(t, v); dbuf_put(b, t, 4); }
void dbuf_u64(dbuf_t *b, uint64_t v) { dbuf_u32(b, (uint32_t)v); dbuf_u32(b, (uint32_t)(v >> 32)); }
void dbuf_u128(dbuf_t *b, uint64_t lo, uint64_t hi) { dbuf_u64(b, lo); dbuf_u64(b, hi); }

void dbuf_str(dbuf_t *b, const char *utf8)
{
    uint16_t u[MTP_STR_MAX_UNITS];
    size_t n = utf8 ? utf8_to_utf16(utf8, u, MTP_STR_MAX_UNITS) : 0;
    if (n == 0) { dbuf_u8(b, 0); return; }
    dbuf_u8(b, (uint8_t)(n + 1));          /* count includes the NUL */
    for (size_t i = 0; i < n; i++) dbuf_u16(b, u[i]);
    dbuf_u16(b, 0);
}

void dbuf_arr16(dbuf_t *b, const uint16_t *a, uint32_t n)
{
    dbuf_u32(b, n);
    for (uint32_t i = 0; i < n; i++) dbuf_u16(b, a[i]);
}

void dbuf_arr32(dbuf_t *b, const uint32_t *a, uint32_t n)
{
    dbuf_u32(b, n);
    for (uint32_t i = 0; i < n; i++) dbuf_u32(b, a[i]);
}

void dbuf_patch_u32(dbuf_t *b, size_t off, uint32_t v)
{
    if (!b->oom && off + 4 <= b->len) wr32(b->data + off, v);
}

/* ---- parser ------------------------------------------------------------- */

void dparse_init(dparse_t *d, const void *p, size_t len)
{
    d->p = p; d->len = len; d->pos = 0; d->err = false;
}

static const uint8_t *take(dparse_t *d, size_t n)
{
    if (d->err || d->pos + n > d->len) { d->err = true; return NULL; }
    const uint8_t *r = d->p + d->pos;
    d->pos += n;
    return r;
}

uint8_t  dparse_u8(dparse_t *d)  { const uint8_t *p = take(d, 1); return p ? p[0] : 0; }
uint16_t dparse_u16(dparse_t *d) { const uint8_t *p = take(d, 2); return p ? rd16(p) : 0; }
uint32_t dparse_u32(dparse_t *d) { const uint8_t *p = take(d, 4); return p ? rd32(p) : 0; }
uint64_t dparse_u64(dparse_t *d) { uint64_t lo = dparse_u32(d); return lo | (uint64_t)dparse_u32(d) << 32; }
void     dparse_skip(dparse_t *d, size_t n) { take(d, n); }

void dparse_str(dparse_t *d, char *out, size_t out_size)
{
    uint16_t u[256];
    uint8_t n = dparse_u8(d);
    for (unsigned i = 0; i < n; i++) u[i] = dparse_u16(d);
    if (d->err) n = 0;
    utf16_to_utf8(u, n, out, out_size);
}
