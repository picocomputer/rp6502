/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* A cursor over a savestate's bytes, and nothing else.
 *
 * Every driver that carries state writes it through one of these and reads it
 * back through another. The walk that hands them out is core/sys/sst.c, which
 * only a machine that makes blobs compiles -- but the save and load bodies
 * themselves sit in the drivers, and those are compiled by every machine that
 * has the driver. So this header stands alone: no osal, no host, and above
 * all no drivers.h, which every one of those drivers already includes.
 *
 * The put and get are inline for the same reason. A firmware that never makes
 * a blob still compiles the bodies, and they must not leave it an undefined
 * reference to a file it does not build.
 *
 * Big-endian on the wire, because rollback netplay requires it and the
 * Pocket's engine already is. Field by field and never a struct copy: no
 * padding, no ABI, no pointer.
 */

#ifndef _CORE_SYS_SST_H_
#define _CORE_SYS_SST_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* at is where the next byte goes, end is one past this row's slot. A put or
 * get that would cross end sets fail and moves nothing; a get answers zero.
 * The walk stops at the first row whose cursor failed, so a body may run to
 * its end without checking after every call. */
typedef struct
{
    uint8_t *at;
    uint8_t *end;
    bool fail;
} sst_cursor_t;

static inline bool sst_ok(const sst_cursor_t *c)
{
    return !c->fail;
}

static inline void sst_fail(sst_cursor_t *c)
{
    c->fail = true;
}

static inline void sst_put(sst_cursor_t *c, const void *src, size_t n)
{
    if (c->fail || (size_t)(c->end - c->at) < n)
    {
        c->fail = true;
        return;
    }
    memcpy(c->at, src, n);
    c->at += n;
}

static inline void sst_get(sst_cursor_t *c, void *dst, size_t n)
{
    if (c->fail || (size_t)(c->end - c->at) < n)
    {
        c->fail = true;
        memset(dst, 0, n);
        return;
    }
    memcpy(dst, c->at, n);
    c->at += n;
}

static inline void sst_put_u8(sst_cursor_t *c, uint8_t v)
{
    sst_put(c, &v, 1);
}

static inline uint8_t sst_get_u8(sst_cursor_t *c)
{
    uint8_t v;
    sst_get(c, &v, 1);
    return v;
}

/* A bool is one byte of 0 or 1. Any other value is a blob this build did not
 * write, so the row that reads one fails rather than store a trap. */
static inline void sst_put_bool(sst_cursor_t *c, bool v)
{
    sst_put_u8(c, v ? 1 : 0);
}

static inline bool sst_get_bool(sst_cursor_t *c)
{
    uint8_t v = sst_get_u8(c);
    if (v > 1)
        c->fail = true;
    return v != 0;
}

static inline void sst_put_u16(sst_cursor_t *c, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    sst_put(c, b, sizeof b);
}

static inline uint16_t sst_get_u16(sst_cursor_t *c)
{
    uint8_t b[2];
    sst_get(c, b, sizeof b);
    return (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
}

static inline void sst_put_u32(sst_cursor_t *c, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    sst_put(c, b, sizeof b);
}

static inline uint32_t sst_get_u32(sst_cursor_t *c)
{
    uint8_t b[4];
    sst_get(c, b, sizeof b);
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | b[3];
}

static inline void sst_put_u64(sst_cursor_t *c, uint64_t v)
{
    sst_put_u32(c, (uint32_t)(v >> 32));
    sst_put_u32(c, (uint32_t)v);
}

static inline uint64_t sst_get_u64(sst_cursor_t *c)
{
    uint64_t hi = sst_get_u32(c);
    return (hi << 32) | sst_get_u32(c);
}

/* The signed widths a machine actually carries, in two's complement, which is
 * what every target here is. */
static inline void sst_put_i16(sst_cursor_t *c, int16_t v) { sst_put_u16(c, (uint16_t)v); }
static inline int16_t sst_get_i16(sst_cursor_t *c) { return (int16_t)sst_get_u16(c); }
static inline void sst_put_i32(sst_cursor_t *c, int32_t v) { sst_put_u32(c, (uint32_t)v); }
static inline int32_t sst_get_i32(sst_cursor_t *c) { return (int32_t)sst_get_u32(c); }
static inline void sst_put_i64(sst_cursor_t *c, int64_t v) { sst_put_u64(c, (uint64_t)v); }
static inline int64_t sst_get_i64(sst_cursor_t *c) { return (int64_t)sst_get_u64(c); }

/* A fixed-width slot holding a NUL-terminated string, zero-filled past it. A
 * string that does not fit fails the cursor rather than truncate: a path cut
 * short names a different file. */
static inline void sst_put_str(sst_cursor_t *c, const char *s, size_t slot)
{
    size_t n = strlen(s);
    if (n >= slot)
    {
        c->fail = true;
        return;
    }
    if (c->fail || (size_t)(c->end - c->at) < slot)
    {
        c->fail = true;
        return;
    }
    memcpy(c->at, s, n);
    memset(c->at + n, 0, slot - n);
    c->at += slot;
}

static inline void sst_get_str(sst_cursor_t *c, char *s, size_t slot)
{
    sst_get(c, s, slot);
    if (s[slot - 1] != 0) /* unterminated: not a string this build wrote */
        c->fail = true;
    s[slot - 1] = 0;
}

/* ---- the blob ------------------------------------------------------------
 *
 * What a machine that makes savestates answers. core/sys/sst.c walks its own
 * roster to build these, so only a machine that compiles that file has them.
 *
 * The two flags are the only thing a caller says about why it is asking, and
 * core states them in its own words: it cannot name libretro's context enum,
 * because libretro.h reaches emu_core from the libretro root alone.
 *
 * TRUSTED  the caller made this blob and is handing it straight back. It may
 *          skip the scratch copy that would otherwise let a failed load undo
 *          itself, because there is nothing to distrust and runahead asks
 *          sixty times a second.
 * SHARED   it crosses to another machine. Every host path is written as zeros
 *          and the load keeps the files and the directory it already has,
 *          because a path under one user's home is a desync under another's.
 *
 * Neither flag means a plain load from a file, which takes the scratch and
 * checks the payload sum. */

#define SST_TRUSTED 0x01
#define SST_SHARED 0x02

/* The size of every blob this build makes, and the only size it will load.
 * A sum of per-row constants, so it is fixed at compile time and answerable
 * before the machine is even up. */
size_t sst_size(void);

/* NULL on success, else a static reason. A save refuses a machine that is
 * mid-fan-out; a load refuses a blob this build did not write, and undoes
 * itself if a row refuses partway through. */
const char *sst_save(void *buf, size_t len, unsigned flags);
const char *sst_load(const void *buf, size_t len, unsigned flags, const char *rom);

#endif /* _CORE_SYS_SST_H_ */
