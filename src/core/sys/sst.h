/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* A cursor over a savestate's bytes.
 *
 * Every driver that carries state writes it through one of these, and a driver
 * is compiled by every machine that lists it -- but core/sys/sst.c, which hands
 * the cursors out row by row, is compiled only by a machine that makes blobs.
 * So the accessors here are inline and this header includes nothing of a
 * machine's own, or a firmware that never saves would be left with undefined
 * references to a file it does not build.
 *
 * The sst_put_uN helpers below write big-endian a field at a time, so a row
 * built from them carries no padding, no ABI and no pointer. A row that hands
 * sst_put a struct instead writes host byte order and that struct's layout,
 * which core/term/term.c does with its term_data_t cells and its uint16_t
 * palette, so those bytes only read back under a build that lays them out the
 * same way.
 */

#ifndef _CORE_SYS_SST_H_
#define _CORE_SYS_SST_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* at is where the next byte goes, end is one past this row's slot. A put that
 * would cross end sets fail and moves nothing; a get that would sets fail and
 * zeros what it was asked to fill, so a body that reads a whole row's worth of
 * fields before checking sst_ok once never acts on an indeterminate value.
 * core/sys/sst.c stops at the first row whose cursor failed. */
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

static inline void sst_put_i16(sst_cursor_t *c, int16_t v) { sst_put_u16(c, (uint16_t)v); }
static inline int16_t sst_get_i16(sst_cursor_t *c) { return (int16_t)sst_get_u16(c); }
static inline void sst_put_i32(sst_cursor_t *c, int32_t v) { sst_put_u32(c, (uint32_t)v); }
static inline int32_t sst_get_i32(sst_cursor_t *c) { return (int32_t)sst_get_u32(c); }
static inline void sst_put_i64(sst_cursor_t *c, int64_t v) { sst_put_u64(c, (uint64_t)v); }
static inline int64_t sst_get_i64(sst_cursor_t *c) { return (int64_t)sst_get_u64(c); }

/* A fixed-width slot holding a NUL-terminated string, zero-filled past it. A
 * string that does not fit fails the cursor rather than truncate, because a
 * path cut short names a different file. */
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
    if (s[slot - 1] != 0)
        c->fail = true;
    s[slot - 1] = 0;
}

/* TRUSTED  the caller made this blob and is handing it straight back, so the
 *          load skips the scratch copy that would otherwise let it undo
 *          itself. There is nothing to distrust, and the copy costs a save on
 *          every load.
 * SHARED   the blob crosses to another machine, so core/api/dir.c writes its
 *          path slots empty and a load keeps the directories and the working
 *          directory it already has, because a path under one user's home is
 *          a desync under another's.
 *
 * Neither flag means a plain load from a file, which takes the scratch and
 * checks the payload sum. */

#define SST_TRUSTED 0x01
#define SST_SHARED 0x02

/* The size of every blob this build makes, and the only size it will load. It
 * is a sum of per-row constants, so it is fixed at compile time and can be
 * answered before the machine is up. */
size_t sst_size(void);

/* NULL on success, else a static reason. A save refuses a machine that is
 * mid-fan-out. A load that a row refuses partway through puts back what it had
 * already written over, but only when the scratch copy was taken, so a
 * SST_TRUSTED load that a row refuses leaves the machine half loaded. */
const char *sst_save(void *buf, size_t len, unsigned flags);
const char *sst_load(const void *buf, size_t len, unsigned flags, const char *rom);

#endif /* _CORE_SYS_SST_H_ */
