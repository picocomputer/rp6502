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
 * which core/term/cells.c does with its mode0_cell_t cells and its uint16_t
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

/* A row's load refuses with a reason, which the load's answer names with
 * the row. Returns false, for the row to return. */
bool sst_refuse(sst_cursor_t *c, const char *why);

static inline void sst_fail(sst_cursor_t *c)
{
    c->fail = true;
}

/* The next n bytes where they lie in the blob, for a save that writes them
 * in place rather than copying them in. NULL and a failed cursor when they
 * would cross the row's end. */
static inline uint8_t *sst_room(sst_cursor_t *c, size_t n)
{
    if (c->fail || (size_t)(c->end - c->at) < n)
    {
        c->fail = true;
        return NULL;
    }
    uint8_t *p = c->at;
    c->at += n;
    return p;
}

/* The next n bytes where they lie in the blob, for a load that takes them
 * as they are rather than copying them aside first. NULL and a failed
 * cursor when they would cross the row's end. */
static inline const uint8_t *sst_view(sst_cursor_t *c, size_t n)
{
    return sst_room(c, n);
}

static inline __attribute__((always_inline)) void sst_put(sst_cursor_t *c, const void *src, size_t n)
{
    uint8_t *p = sst_room(c, n);
    if (p)
        memcpy(p, src, n);
}

static inline __attribute__((always_inline)) void sst_get(sst_cursor_t *c, void *dst, size_t n)
{
    const uint8_t *p = sst_view(c, n);
    if (p)
        memcpy(dst, p, n);
    else
        memset(dst, 0, n);
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

/* A word of the blob as the machine reads it, its first byte the low one,
 * which core/sys/sst.c asserts. */
typedef uint32_t sst_word_t __attribute__((may_alias));

static inline __attribute__((always_inline)) uint32_t
sst_swap32(uint32_t v)
{
    return v >> 24 | (v >> 8 & 0xFF00u) | (v << 8 & 0xFF0000u) | v << 24;
}

/* The big-endian word at p. One on a word boundary is one access, which a
 * machine whose blob is in slow memory needs: the Pocket's is past its
 * cache. */
static inline __attribute__((always_inline)) uint32_t
sst_be32_read(const uint8_t *p)
{
    if (((uintptr_t)p & 3u) == 0)
        return sst_swap32(*(const sst_word_t *)p);
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static inline __attribute__((always_inline)) void
sst_be32_write(uint8_t *p, uint32_t v)
{
    if (((uintptr_t)p & 3u) == 0)
    {
        *(sst_word_t *)p = sst_swap32(v);
        return;
    }
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static inline void sst_put_u32(sst_cursor_t *c, uint32_t v)
{
    uint8_t *p = sst_room(c, 4);
    if (p)
        sst_be32_write(p, v);
}

static inline uint32_t sst_get_u32(sst_cursor_t *c)
{
    const uint8_t *p = sst_view(c, 4);
    return p ? sst_be32_read(p) : 0;
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
    uint8_t *p = sst_room(c, slot);
    if (p)
    {
        memcpy(p, s, n);
        memset(p + n, 0, slot - n);
    }
}

static inline void sst_get_str(sst_cursor_t *c, char *s, size_t slot)
{
    sst_get(c, s, slot);
    if (s[slot - 1] != 0)
        c->fail = true;
    s[slot - 1] = 0;
}

/* A slot sst_put_str wrote, as the string where it lies in the blob. A slot
 * whose last byte is not NUL fails the cursor and reads as "", as one that
 * would cross the row's end does. */
static inline const char *sst_view_str(sst_cursor_t *c, size_t slot)
{
    const uint8_t *p = sst_view(c, slot);
    if (!p || p[slot - 1] != 0)
    {
        c->fail = true;
        return "";
    }
    return (const char *)p;
}

/* TRUSTED  the caller made this blob and loads it right back, so the load
 *          skips the payload sum. The blob is known to be good, and the sum
 *          costs a pass over it on every load.
 * SHARED   the blob is sent to another machine for netplay. core/api/dir.c
 *          writes its path slots empty, and a load leaves the open
 *          directories and the working directory as they are, because a path
 *          under one player's home directory may not exist on the other
 *          player's machine, and the two machines would fall out of sync. For
 *          the same reason, core/api/std.c does not record files opened by
 *          path, and a load closes them; a SAVE: file is recorded by its
 *          name. A SHARED load skips the payload sum too.
 *
 * Neither flag means a plain load from a file, which checks the payload
 * sum. */

#define SST_TRUSTED 0x01
#define SST_SHARED 0x02

/* One driver's chunk, as its row in the driver list names it:
 *
 *   SST(.id = "TTY_",
 *       .version = 1,
 *       .size = TTY_SST_SIZE,
 *       .save = tty_sst_save,
 *       .load = tty_sst_load)
 *
 * The id is four letters. The version goes up whenever what save writes
 * changes, and size is the most it can write. */
typedef struct
{
    char id[5];
    uint16_t version;
    uint32_t size;
    void (*save)(sst_cursor_t *c, unsigned flags);
    bool (*load)(sst_cursor_t *c, unsigned flags);
} sst_row_t;

/* The size of every blob this build makes, and the only size it will load. It
 * is a sum of the rows' sizes, so it is fixed for a build and can be
 * answered before the machine is up. */
size_t sst_size(void);

/* NULL on success, else a static reason. A save refuses a machine that is
 * mid-fan-out.
 *
 * A load overwrites the machine one row at a time, so a row that refuses
 * partway through leaves the rows before it already loaded. undo, sst_size()
 * bytes of the caller's, is where the load first saves the machine as it
 * stands, and a refused load puts it back from there. A caller with no undo
 * passes NULL and is left with a half loaded machine when a row refuses. */
const char *sst_save(void *buf, size_t len, unsigned flags);
const char *sst_load(const void *buf, size_t len, void *undo, unsigned flags);

/* sst_load without the checks, for a caller that has just had sst_check pass
 * the same bytes with the same flags and has work to do in between. */
const char *sst_load_checked(const void *buf, void *undo, unsigned flags);

/* NULL when the len bytes at buf are a savestate this build can load, else
 * a static reason. It checks what sst_load checks before it changes
 * anything, so a frontend can answer for a blob before it loads it. */
const char *sst_check(const void *buf, size_t len, unsigned flags);

#endif /* _CORE_SYS_SST_H_ */
