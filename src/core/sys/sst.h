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
 * which core/vga/mode/mode0.c does with its mode0_cell_t cells and colors, so
 * those bytes only read back under a build that lays them out the same way.
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
    const char *why; /* a refusal's reason, or NULL */
} sst_cursor_t;

static inline bool sst_ok(const sst_cursor_t *c)
{
    return !c->fail;
}

/* A row's load refuses with a reason, which the load's answer names with
 * the row. Returns false, for the row to return. */
static inline bool sst_refuse(sst_cursor_t *c, const char *why)
{
    c->fail = true;
    c->why = why;
    return false;
}

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

/* A driver's chunk, made by SST_TABLE from a list of its state. */
typedef struct
{
    uint32_t size;    /* the most save writes */
    const char *desc; /* every saved row's kind and name, for the manifest */
    void (*save)(sst_cursor_t *c, unsigned flags);
    bool (*load)(sst_cursor_t *c, unsigned flags);
} sst_table_t;

/* One driver's chunk, as its row in the driver list names it:
 *
 *   SST("TTY_", tty)
 *
 * The id is four letters, and tty_sst is the table SST_TABLE(tty, ...)
 * made. The chunk's version is a sum of the table's description, so any
 * change to its list changes it. */
typedef struct
{
    char id[5];
    const sst_table_t *table;
} sst_row_t;

/* A driver's state, as a list in its .c, under the statics it covers:
 *
 *   static m6522_t via;
 *   static uint8_t latch;
 *   static uint8_t scratch;
 *
 *   #define VIA_SST(X)                              \
 *       X(STRUCT, via.pa, VIA_PORT_ROWS)            \
 *       X(U8, latch)                                \
 *       X(SKIP, scratch, "rebuilt on every write")
 *   SST_TABLE(via, VIA_SST)
 *
 * SST_TABLE(via, VIA_SST) makes const sst_table_t via_sst: a save and a load
 * that take the rows in order, the most the save writes, and the description
 * the manifest sums. Every row may end with a comment, which is ignored.
 *
 *   U8, U16, U32, U64, I16, I32, I64, BOOL  lv
 *       a scalar at that width, big-endian. The build fails unless lv is
 *       that size, so a field that is widened cannot be cut short.
 *   BYTES lv              an array of bytes, as they are
 *   U8S, U16S, U32S, I32S, BOOLS lv
 *                         an array of those, an element at a time; U8S is
 *                         for a volatile array, which BYTES cannot copy
 *   STR lv                a char array holding a NUL-terminated string
 *   STRUCT lv, ROWS       a struct, by ROWS(Y, lv), a list of its members
 *                         made the same way, with Y in place of X:
 *                           #define VIA_PORT_ROWS(Y, s) \
 *                               Y(U8, s.inpr) Y(BOOL, s.c1_in)
 *   STRUCTS lv, ROWS      an array of such structs
 *   FN put, get, size, version
 *       a hand-written part: put(c, flags) writes at most size bytes and
 *       get(c, flags) reads them back and returns false to refuse. version
 *       goes up whenever what put writes changes, because the description
 *       cannot see inside it.
 *   VALIDATE fn           bool fn(sst_cursor_t *c), run at that point of the
 *                         load, after the rows above it; false, or a
 *                         sst_refuse, refuses the load. It may also finish
 *                         the load, rebuilding what derives from the rows.
 *   IN_FN lv              state a FN row carries
 *   SKIP lv               state the savestate does not carry
 *   SKIP_PREFIX prefix    every static the unit names with that prefix,
 *                         for statics a macro makes, such as config.c's
 *                         settings, which the driver walk makes one of each
 *
 * A table whose rows are all SKIP is not in the driver list, and its file
 * needs no driver for it: it is there for the inventory check, which builds
 * every unit with SST_CHECK and requires each static and global of the unit
 * to be the first name of one of its table's rows.
 *
 * SST_TABLE(name, ...) defines name_sst_put, name_sst_get and, under
 * SST_CHECK, name_sst_check, so a unit's own FN and VALIDATE functions take
 * other names, such as name_sst_valid.
 *
 * lv is anything that can be assigned and measured: a static, a member of
 * one, or a hardware register. The load writes each row straight into it, so
 * a load that refuses partway leaves rows written, and sys_reset follows. */

#define SST_ROW_PUT(kind, ...) SST_PUT_##kind(__VA_ARGS__)
#define SST_ROW_GET(kind, ...) SST_GET_##kind(__VA_ARGS__)
#define SST_ROW_SIZE(kind, ...) SST_SIZE_##kind(__VA_ARGS__)
#define SST_ROW_DESC(kind, ...) SST_DESC_##kind(__VA_ARGS__)
#define SST_ROW_ASSERT(kind, ...) SST_ASSERT_##kind(__VA_ARGS__)
#define SST_ROW_SKIPS(kind, ...) SST_SKIPS_##kind(__VA_ARGS__)
/* The same, for the members of a STRUCT, because a macro does not expand
 * inside its own expansion. */
#define SST_ROW_PUT2(kind, ...) SST_PUT_##kind(__VA_ARGS__)
#define SST_ROW_GET2(kind, ...) SST_GET_##kind(__VA_ARGS__)
#define SST_ROW_SIZE2(kind, ...) SST_SIZE_##kind(__VA_ARGS__)
#define SST_ROW_DESC2(kind, ...) SST_DESC_##kind(__VA_ARGS__)
#define SST_ROW_ASSERT2(kind, ...) SST_ASSERT_##kind(__VA_ARGS__)
#define SST_ROW_SKIPS2(kind, ...) SST_SKIPS_##kind(__VA_ARGS__)

#define SST_SCALAR(name, n, lv)                            \
    _Static_assert(sizeof(lv) == (n), #lv " is not " #n " bytes");
#define SST_PUT_U8(lv, ...) sst_put_u8(c, (uint8_t)(lv));
#define SST_GET_U8(lv, ...) (lv) = sst_get_u8(c);
#define SST_SIZE_U8(lv, ...) +1
#define SST_DESC_U8(lv, ...) "U8 " #lv ";"
#define SST_ASSERT_U8(lv, ...) SST_SCALAR(U8, 1, lv)
#define SST_SKIPS_U8(lv, ...)
#define SST_PUT_U16(lv, ...) sst_put_u16(c, (uint16_t)(lv));
#define SST_GET_U16(lv, ...) (lv) = sst_get_u16(c);
#define SST_SIZE_U16(lv, ...) +2
#define SST_DESC_U16(lv, ...) "U16 " #lv ";"
#define SST_ASSERT_U16(lv, ...) SST_SCALAR(U16, 2, lv)
#define SST_SKIPS_U16(lv, ...)
#define SST_PUT_U32(lv, ...) sst_put_u32(c, (uint32_t)(lv));
#define SST_GET_U32(lv, ...) (lv) = sst_get_u32(c);
#define SST_SIZE_U32(lv, ...) +4
#define SST_DESC_U32(lv, ...) "U32 " #lv ";"
#define SST_ASSERT_U32(lv, ...) SST_SCALAR(U32, 4, lv)
#define SST_SKIPS_U32(lv, ...)
#define SST_PUT_U64(lv, ...) sst_put_u64(c, (uint64_t)(lv));
#define SST_GET_U64(lv, ...) (lv) = sst_get_u64(c);
#define SST_SIZE_U64(lv, ...) +8
#define SST_DESC_U64(lv, ...) "U64 " #lv ";"
#define SST_ASSERT_U64(lv, ...) SST_SCALAR(U64, 8, lv)
#define SST_SKIPS_U64(lv, ...)
#define SST_PUT_I16(lv, ...) sst_put_i16(c, (int16_t)(lv));
#define SST_GET_I16(lv, ...) (lv) = sst_get_i16(c);
#define SST_SIZE_I16(lv, ...) +2
#define SST_DESC_I16(lv, ...) "I16 " #lv ";"
#define SST_ASSERT_I16(lv, ...) SST_SCALAR(I16, 2, lv)
#define SST_SKIPS_I16(lv, ...)
#define SST_PUT_I32(lv, ...) sst_put_i32(c, (int32_t)(lv));
#define SST_GET_I32(lv, ...) (lv) = sst_get_i32(c);
#define SST_SIZE_I32(lv, ...) +4
#define SST_DESC_I32(lv, ...) "I32 " #lv ";"
#define SST_ASSERT_I32(lv, ...) SST_SCALAR(I32, 4, lv)
#define SST_SKIPS_I32(lv, ...)
#define SST_PUT_I64(lv, ...) sst_put_i64(c, (int64_t)(lv));
#define SST_GET_I64(lv, ...) (lv) = sst_get_i64(c);
#define SST_SIZE_I64(lv, ...) +8
#define SST_DESC_I64(lv, ...) "I64 " #lv ";"
#define SST_ASSERT_I64(lv, ...) SST_SCALAR(I64, 8, lv)
#define SST_SKIPS_I64(lv, ...)
#define SST_PUT_BOOL(lv, ...) sst_put_bool(c, (lv));
#define SST_GET_BOOL(lv, ...) (lv) = sst_get_bool(c);
#define SST_SIZE_BOOL(lv, ...) +1
#define SST_DESC_BOOL(lv, ...) "BOOL " #lv ";"
#define SST_ASSERT_BOOL(lv, ...) SST_SCALAR(BOOL, sizeof(bool), lv)
#define SST_SKIPS_BOOL(lv, ...)

#define SST_COUNT(lv) (sizeof(lv) / sizeof((lv)[0]))
#define SST_EACH(lv) for (size_t sst_i = 0; sst_i < SST_COUNT(lv); sst_i++)
#define SST_PUT_BYTES(lv, ...) sst_put(c, (lv), sizeof(lv));
#define SST_GET_BYTES(lv, ...) sst_get(c, (lv), sizeof(lv));
#define SST_SIZE_BYTES(lv, ...) +sizeof(lv)
#define SST_DESC_BYTES(lv, ...) "BYTES " #lv ";"
#define SST_ASSERT_BYTES(lv, ...) SST_SCALAR(BYTES, 1, (lv)[0])
#define SST_SKIPS_BYTES(lv, ...)
#define SST_PUT_U8S(lv, ...) SST_EACH(lv) sst_put_u8(c, (lv)[sst_i]);
#define SST_GET_U8S(lv, ...) SST_EACH(lv) (lv)[sst_i] = sst_get_u8(c);
#define SST_SIZE_U8S(lv, ...) +SST_COUNT(lv)
#define SST_DESC_U8S(lv, ...) "U8S " #lv ";"
#define SST_ASSERT_U8S(lv, ...) SST_SCALAR(U8S, 1, (lv)[0])
#define SST_SKIPS_U8S(lv, ...)
#define SST_PUT_I32S(lv, ...) SST_EACH(lv) sst_put_i32(c, (lv)[sst_i]);
#define SST_GET_I32S(lv, ...) SST_EACH(lv) (lv)[sst_i] = sst_get_i32(c);
#define SST_SIZE_I32S(lv, ...) +4 * SST_COUNT(lv)
#define SST_DESC_I32S(lv, ...) "I32S " #lv ";"
#define SST_ASSERT_I32S(lv, ...) SST_SCALAR(I32S, 4, (lv)[0])
#define SST_SKIPS_I32S(lv, ...)
#define SST_PUT_U16S(lv, ...) SST_EACH(lv) sst_put_u16(c, (lv)[sst_i]);
#define SST_GET_U16S(lv, ...) SST_EACH(lv) (lv)[sst_i] = sst_get_u16(c);
#define SST_SIZE_U16S(lv, ...) +2 * SST_COUNT(lv)
#define SST_DESC_U16S(lv, ...) "U16S " #lv ";"
#define SST_ASSERT_U16S(lv, ...) SST_SCALAR(U16S, 2, (lv)[0])
#define SST_SKIPS_U16S(lv, ...)
#define SST_PUT_U32S(lv, ...) SST_EACH(lv) sst_put_u32(c, (lv)[sst_i]);
#define SST_GET_U32S(lv, ...) SST_EACH(lv) (lv)[sst_i] = sst_get_u32(c);
#define SST_SIZE_U32S(lv, ...) +4 * SST_COUNT(lv)
#define SST_DESC_U32S(lv, ...) "U32S " #lv ";"
#define SST_ASSERT_U32S(lv, ...) SST_SCALAR(U32S, 4, (lv)[0])
#define SST_SKIPS_U32S(lv, ...)
#define SST_PUT_BOOLS(lv, ...) SST_EACH(lv) sst_put_bool(c, (lv)[sst_i]);
#define SST_GET_BOOLS(lv, ...) SST_EACH(lv) (lv)[sst_i] = sst_get_bool(c);
#define SST_SIZE_BOOLS(lv, ...) +SST_COUNT(lv)
#define SST_DESC_BOOLS(lv, ...) "BOOLS " #lv ";"
#define SST_ASSERT_BOOLS(lv, ...) SST_SCALAR(BOOLS, sizeof(bool), (lv)[0])
#define SST_SKIPS_BOOLS(lv, ...)
#define SST_PUT_STR(lv, ...) sst_put_str(c, (lv), sizeof(lv));
#define SST_GET_STR(lv, ...) sst_get_str(c, (lv), sizeof(lv));
#define SST_SIZE_STR(lv, ...) +sizeof(lv)
#define SST_DESC_STR(lv, ...) "STR " #lv ";"
#define SST_ASSERT_STR(lv, ...) SST_SCALAR(STR, 1, (lv)[0])
#define SST_SKIPS_STR(lv, ...)

#define SST_PUT_STRUCT(lv, ROWS, ...) ROWS(SST_ROW_PUT2, lv)
#define SST_GET_STRUCT(lv, ROWS, ...) ROWS(SST_ROW_GET2, lv)
#define SST_SIZE_STRUCT(lv, ROWS, ...) ROWS(SST_ROW_SIZE2, lv)
#define SST_DESC_STRUCT(lv, ROWS, ...) "{" ROWS(SST_ROW_DESC2, lv) "}"
#define SST_ASSERT_STRUCT(lv, ROWS, ...) ROWS(SST_ROW_ASSERT2, lv)
#define SST_SKIPS_STRUCT(lv, ROWS, ...) ROWS(SST_ROW_SKIPS2, lv)
#define SST_PUT_STRUCTS(lv, ROWS, ...) \
    SST_EACH(lv) { ROWS(SST_ROW_PUT2, (lv)[sst_i]) }
#define SST_GET_STRUCTS(lv, ROWS, ...) \
    SST_EACH(lv) { ROWS(SST_ROW_GET2, (lv)[sst_i]) }
#define SST_SIZE_STRUCTS(lv, ROWS, ...) \
    +SST_COUNT(lv) * (0 ROWS(SST_ROW_SIZE2, (lv)[0]))
#define SST_DESC_STRUCTS(lv, ROWS, ...) \
    "STRUCTS " #lv "{" ROWS(SST_ROW_DESC2, (lv)[0]) "}"
#define SST_ASSERT_STRUCTS(lv, ROWS, ...) ROWS(SST_ROW_ASSERT2, (lv)[0])
#define SST_SKIPS_STRUCTS(lv, ROWS, ...) ROWS(SST_ROW_SKIPS2, (lv)[0])

#define SST_PUT_FN(put, get, size, version, ...) put(c, flags);
#define SST_GET_FN(put, get, size, version, ...) \
    if (!get(c, flags))                          \
        return false;
#define SST_SIZE_FN(put, get, size, version, ...) +(size)
#define SST_DESC_FN(put, get, size, version, ...) \
    "FN " #put " " #get " " #version ";"
#define SST_ASSERT_FN(put, get, size, version, ...)
#define SST_SKIPS_FN(put, get, size, version, ...)

#define SST_PUT_VALIDATE(fn, ...)
#define SST_GET_VALIDATE(fn, ...) \
    if (!sst_ok(c) || !fn(c))     \
        return false;
#define SST_SIZE_VALIDATE(fn, ...)
#define SST_DESC_VALIDATE(fn, ...)
#define SST_ASSERT_VALIDATE(fn, ...)
#define SST_SKIPS_VALIDATE(fn, ...)

#define SST_PUT_IN_FN(lv, ...)
#define SST_GET_IN_FN(lv, ...)
#define SST_SIZE_IN_FN(lv, ...)
#define SST_DESC_IN_FN(lv, ...)
#define SST_ASSERT_IN_FN(lv, ...)
#define SST_SKIPS_IN_FN(lv, ...) "IN_FN " #lv ";"

#define SST_PUT_SKIP_PREFIX(prefix, ...)
#define SST_GET_SKIP_PREFIX(prefix, ...)
#define SST_SIZE_SKIP_PREFIX(prefix, ...)
#define SST_DESC_SKIP_PREFIX(prefix, ...)
#define SST_ASSERT_SKIP_PREFIX(prefix, ...)
#define SST_SKIPS_SKIP_PREFIX(prefix, ...) "SKIP_PREFIX " #prefix ";"

#define SST_PUT_SKIP(lv, ...)
#define SST_GET_SKIP(lv, ...)
#define SST_SIZE_SKIP(lv, ...)
#define SST_DESC_SKIP(lv, ...)
#define SST_ASSERT_SKIP(lv, ...)
#define SST_SKIPS_SKIP(lv, ...) "SKIP " #lv ";"

/* What the inventory check reads out of the object: the table's name, its
 * saved rows and its other rows, between @ marks. */
#ifdef SST_CHECK
#define SST_TABLE_CHECK(name, LIST)                                   \
    __attribute__((used)) static const char name##_sst_check[] =     \
        "@SST@" #name "@" LIST(SST_ROW_DESC) "@" LIST(SST_ROW_SKIPS) "@";
#else
#define SST_TABLE_CHECK(name, LIST)
#endif

#define SST_TABLE(name, LIST)                                         \
    LIST(SST_ROW_ASSERT)                                              \
    static void name##_sst_put(sst_cursor_t *c, unsigned flags)      \
    {                                                                 \
        (void)c;                                                      \
        (void)flags;                                                  \
        LIST(SST_ROW_PUT)                                             \
    }                                                                 \
    static bool name##_sst_get(sst_cursor_t *c, unsigned flags)      \
    {                                                                 \
        (void)flags;                                                  \
        LIST(SST_ROW_GET)                                             \
        return sst_ok(c);                                             \
    }                                                                 \
    const sst_table_t name##_sst = {                                  \
        .size = 0 LIST(SST_ROW_SIZE),                                 \
        .desc = "" LIST(SST_ROW_DESC),                                \
        .save = name##_sst_put,                                       \
        .load = name##_sst_get};                                      \
    SST_TABLE_CHECK(name, LIST)

/* The size of every blob this build makes, and the only size it will load. It
 * is a sum of the rows' sizes, so it is fixed for a build and can be
 * answered before the machine is up. */
size_t sst_size(void);

/* NULL on success, else a static reason. A save refuses a machine that is
 * mid-fan-out.
 *
 * A load overwrites the machine one row at a time, so a row that refuses
 * partway through leaves the rows before it loaded, itself partly loaded and
 * the rows after it as they were. A refused load therefore resets the machine
 * with sys_reset before it returns: the machine is stopped and empty, and
 * the caller starts a program on it again if it has one. A blob refused
 * before any row loads changes nothing. */
const char *sst_save(void *buf, size_t len, unsigned flags);
const char *sst_load(const void *buf, size_t len, unsigned flags);

/* sst_load without the checks, for a caller that has just had sst_check pass
 * the same bytes with the same flags and has work to do in between. */
const char *sst_load_checked(const void *buf, unsigned flags);

/* NULL when the len bytes at buf are a savestate this build can load, else
 * a static reason. It checks what sst_load checks before it changes
 * anything, so a frontend can answer for a blob before it loads it. */
const char *sst_check(const void *buf, size_t len, unsigned flags);

#endif /* _CORE_SYS_SST_H_ */
