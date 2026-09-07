/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The savestate walk: this machine's roster, turned into a blob and back.
 *
 * A driver says what it holds in the ninth column of its row, and this is the
 * only file that reads that column. Every other consumer of a row -- the four
 * fan-outs in sys.c, the config machinery, the Pico's monitor -- discards the
 * tail, which is why a machine that never makes a blob still compiles every
 * row unchanged and never refers to a save or load body by name.
 *
 * The layout is fixed at compile time. Every chunk has a slot of a size its
 * driver declares, at an offset the roster order decides, and a row that
 * writes less than its slot is zero-padded to the end of it. Nothing here is
 * variable, so a frontend that allocates once always allocates enough, and a
 * rewind that deltas one state against the next sees a static region stay
 * static.
 *
 * Nothing in this file may reach the osal or a host seam beyond host_crc32.
 * tests/host/emu/test_roster.c compiles it against a machine made of nothing
 * and links no library at all.
 */

#include "core/sys/sst.h"

#include "core/api/std.h"
#include "core/sys/sys.h"
#include "core/wdc/resb.h"
#include "host/host.h"

/* The machine's own constants, because a row's slot size may be built from
 * them: the console's ring is this machine's size and the terminal's height
 * is too. Then the roster, which names every row. */
#include "machine.h"
#include "drivers.h"

#include <string.h>

/* ---- the shape ----------------------------------------------------------- */

#define SST_MAGIC "RP65"
#define SST_END_MAGIC "56PR" /* the reverse, so a truncated tail is not a header */
#define SST_FORMAT 1

/* magic 4, format 2, count 2, total 4, manifest 4, latch 2 */
#define SST_HEADER_LEN 18
/* id 4, version 2, used 4 */
#define SST_CHUNK_HDR 10
/* payload sum 4, end magic 4 */
#define SST_TRAILER_LEN 8

/* The five expansions below are fixed at nine parameters where every other
 * consumer of DRIVER is variadic. That is the tripwire: this file is the only
 * one that binds the ninth column, so a row written with eight would compile
 * everywhere else and silently contribute no chunk. Fixed arity turns that
 * into an error here instead of a machine that loads back missing a driver.
 *
 * Every chunk's slot and header, summed as one expression so the total is a
 * compile-time constant. A row with no state expands to nothing at all. */
#define DRIVER(i, t, iot, r, s, b, c1, c2, sst) sst
#define SST(id, ver, size, sv, ld) +(SST_CHUNK_HDR + (size))
enum
{
    SST_PAYLOAD_LEN = 0 DRIVERS_FORWARD(RP6502_MACH_DRIVERS)
};
#undef SST
#define SST(id, ver, size, sv, ld) +1
enum
{
    SST_COUNT = 0 DRIVERS_FORWARD(RP6502_MACH_DRIVERS)
};
#undef SST
#undef DRIVER

#define SST_TOTAL (SST_HEADER_LEN + SST_PAYLOAD_LEN + SST_TRAILER_LEN)

size_t sst_size(void)
{
    return SST_TOTAL;
}

/* The undo a late refusal needs. A load overwrites the machine row by row, so
 * a row that refuses partway through has already had rows before it applied.
 * This holds the machine as it stood when the walk began.
 *
 * A static rather than an allocation: the one path whose whole job is not to
 * destroy the machine must not be able to fail for want of memory. */
static uint8_t sst_scratch[SST_TOTAL];

/* ---- the manifest -------------------------------------------------------- */

/* Every row's id, version and slot size, hashed in roster order. It is the
 * whole of what makes one build's blob legible to another: a machine whose
 * roster differs in any of the three refuses the blob rather than walk it
 * into the wrong rows.
 *
 * RP6502_STD_DRIVERS goes in too. The STD chunk writes a driver index, and an
 * index means nothing to a machine that lists its stdio drivers differently.
 *
 * Computed on first use, because it needs statements and the total does not. */
static uint32_t sst_manifest_crc;
static bool sst_manifest_done;

static uint32_t sst_manifest_step(uint32_t crc, const char *id, uint16_t ver, uint32_t size)
{
    uint8_t row[10];
    memcpy(row, id, 4);
    row[4] = (uint8_t)(ver >> 8);
    row[5] = (uint8_t)ver;
    row[6] = (uint8_t)(size >> 24);
    row[7] = (uint8_t)(size >> 16);
    row[8] = (uint8_t)(size >> 8);
    row[9] = (uint8_t)size;
    return host_crc32(crc, row, sizeof row);
}

static uint32_t sst_manifest(void)
{
    if (!sst_manifest_done)
    {
        uint32_t crc = 0;
#define DRIVER(i, t, iot, r, s, b, c1, c2, sst) sst
#define SST(id, ver, size, sv, ld)                              \
    _Static_assert(sizeof #id == 5, "a chunk id is four bytes"); \
    crc = sst_manifest_step(crc, #id, (ver), (size));
        DRIVERS_FORWARD(RP6502_MACH_DRIVERS)
#undef SST
#undef DRIVER
        /* The stdio table's own shape, since a descriptor's driver index is
         * only a number against this list. */
        size_t std_count;
        (void)std_drivers(&std_count);
        uint8_t n = (uint8_t)std_count;
        crc = host_crc32(crc, &n, 1);
        sst_manifest_crc = crc;
        sst_manifest_done = true;
    }
    return sst_manifest_crc;
}

/* ---- the walks ----------------------------------------------------------- */

/* One row's slot, handed out with its own end so a body that reads less than
 * it wrote cannot walk into the row after it. */
static uint8_t *sst_body;   /* the payload area */
static size_t sst_offset;   /* where the next chunk header goes inside it */
static bool sst_walk_bad;
static const char *sst_why;

static void sst_row_save(const char *id, uint16_t ver, uint32_t size,
                         void (*save)(sst_cursor_t *, unsigned), unsigned flags)
{
    if (sst_walk_bad)
        return;
    uint8_t *p = sst_body + sst_offset;
    memcpy(p, id, 4);
    uint8_t *payload = p + SST_CHUNK_HDR;
    sst_cursor_t c = {payload, payload + size, false};
    save(&c, flags);
    if (!sst_ok(&c))
    {
        sst_walk_bad = true;
        sst_why = "a driver could not say what it holds";
        return;
    }
    uint32_t used = (uint32_t)(c.at - payload);
    p[4] = (uint8_t)(ver >> 8);
    p[5] = (uint8_t)ver;
    p[6] = (uint8_t)(used >> 24);
    p[7] = (uint8_t)(used >> 16);
    p[8] = (uint8_t)(used >> 8);
    p[9] = (uint8_t)used;
    /* Zeroed to the end of the slot, not merely left. A rewind deltas one
     * state against the next word by word, and a tail carrying whatever the
     * frontend's buffer held would make a still region move every frame. */
    memset(c.at, 0, size - used);
    sst_offset += SST_CHUNK_HDR + size;
}

static void sst_row_load(const char *id, uint16_t ver, uint32_t size,
                         bool (*load)(sst_cursor_t *, unsigned), unsigned flags)
{
    if (sst_walk_bad)
        return;
    const uint8_t *p = sst_body + sst_offset;
    uint32_t used = ((uint32_t)p[6] << 24) | ((uint32_t)p[7] << 16) |
                    ((uint32_t)p[8] << 8) | p[9];
    uint16_t got = (uint16_t)((p[4] << 8) | p[5]);
    /* The manifest has already agreed on every id, version and size, so these
     * two can only differ in a blob that was edited after it was written. */
    if (memcmp(p, id, 4) != 0 || got != ver || used > size)
    {
        sst_walk_bad = true;
        sst_why = "a chunk is not the one the manifest promised";
        return;
    }
    uint8_t *payload = (uint8_t *)p + SST_CHUNK_HDR;
    sst_cursor_t c = {payload, payload + used, false};
    if (!load(&c, flags) || !sst_ok(&c))
    {
        sst_walk_bad = true;
        sst_why = "a driver refused what it was handed";
        return;
    }
    sst_offset += SST_CHUNK_HDR + size;
}

/* ---- header and trailer --------------------------------------------------- */

static void sst_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t sst_get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* The whole blob up to the sum itself, header and latch included. Nothing
 * outside this core checks a savestate's integrity, so this is the only
 * check there is. */
static uint32_t sst_payload_crc(const uint8_t *buf)
{
    return host_crc32(0, buf, SST_TOTAL - SST_TRAILER_LEN);
}

static const char *sst_write(uint8_t *buf, unsigned flags)
{
    sys_latch_t latch;
    sys_latch_get(&latch);
    /* Only a machine that has committed. starting and stopping are moments
     * inside sys_commit and a blob holding one could not be loaded back. */
    if (latch.state != 0 && latch.state != 2)
        return "the machine is mid-fan-out";

    memcpy(buf, SST_MAGIC, 4);
    buf[4] = 0;
    buf[5] = SST_FORMAT;
    buf[6] = (uint8_t)(SST_COUNT >> 8);
    buf[7] = (uint8_t)SST_COUNT;
    sst_put32(buf + 8, SST_TOTAL);
    sst_put32(buf + 12, sst_manifest());
    buf[16] = latch.state;
    buf[17] = (uint8_t)((latch.held ? 1 : 0) | (latch.breaking ? 2 : 0));

    sst_body = buf + SST_HEADER_LEN;
    sst_offset = 0;
    sst_walk_bad = false;
    sst_why = NULL;
#define DRIVER(i, t, iot, r, s, b, c1, c2, sst) sst
#define SST(id, ver, size, sv, ld) sst_row_save(#id, (ver), (size), sv, flags);
    DRIVERS_FORWARD(RP6502_MACH_DRIVERS)
#undef SST
#undef DRIVER
    if (sst_walk_bad)
        return sst_why;

    sst_put32(buf + SST_TOTAL - SST_TRAILER_LEN, sst_payload_crc(buf));
    memcpy(buf + SST_TOTAL - 4, SST_END_MAGIC, 4);
    return NULL;
}

const char *sst_save(void *buf, size_t len, unsigned flags)
{
    if (len < SST_TOTAL)
        return "the buffer is too small";
    return sst_write((uint8_t *)buf, flags);
}

const char *sst_load(const void *buf, size_t len, unsigned flags, const char *rom)
{
    (void)rom; /* the rows that reopen files take it from here once they exist */
    const uint8_t *p = (const uint8_t *)buf;

    if (len < SST_HEADER_LEN)
        return "too short to be a savestate";
    if (memcmp(p, SST_MAGIC, 4) != 0)
        return "not a savestate";
    if (p[4] != 0 || p[5] != SST_FORMAT)
        return "a savestate format this build does not know";
    /* Never len: a frontend hands over the length it recorded when the file
     * was written, which is another build's. */
    uint32_t total = sst_get32(p + 8);
    if (total != SST_TOTAL || len < total)
        return "a savestate of a different size";
    if (((uint32_t)p[6] << 8 | p[7]) != SST_COUNT)
        return "a savestate with a different chunk count";
    if (sst_get32(p + 12) != sst_manifest())
        return "a savestate from a build whose drivers differ";
    if (memcmp(p + SST_TOTAL - 4, SST_END_MAGIC, 4) != 0)
        return "a savestate that does not end where it should";

    sys_latch_t latch;
    latch.state = p[16];
    latch.held = (p[17] & 1) != 0;
    latch.breaking = (p[17] & 2) != 0;
    if ((p[17] & ~(uint8_t)3) || (latch.state != 0 && latch.state != 2) ||
        (latch.state == 0 && !latch.held))
        return "a savestate of a machine that could not have existed";

    /* A blob that never touched a disk is checked by whatever carried it. */
    if (!flags && sst_get32(p + SST_TOTAL - SST_TRAILER_LEN) != sst_payload_crc(p))
        return "a savestate that does not add up";

    /* The undo, before the first row is written over. A caller that made this
     * blob itself has nothing to undo to. */
    bool guarded = !(flags & SST_TRUSTED);
    if (guarded && sst_write(sst_scratch, flags) != NULL)
        return "the machine could not be copied aside";

    /* Before the walk, never after: the only other way to put RESB down also
     * resets the 6502, the 6522, the parked bus and the run clock, and those
     * four rows are the last of the roster. */
    if (!sys_latch_apply(&latch))
        return "a savestate of a machine that could not have existed";

    sst_body = (uint8_t *)(uintptr_t)(p + SST_HEADER_LEN);
    sst_offset = 0;
    sst_walk_bad = false;
    sst_why = NULL;
#define DRIVER(i, t, iot, r, s, b, c1, c2, sst) sst
#define SST(id, ver, size, sv, ld) sst_row_load(#id, (ver), (size), ld, flags);
    DRIVERS_FORWARD(RP6502_MACH_DRIVERS)
#undef SST
#undef DRIVER
    if (!sst_walk_bad)
        return NULL;

    const char *why = sst_why;
    if (guarded)
    {
        /* Put back what the walk had already written over. The scratch is
         * this build's own blob, so nothing about it can be refused. */
        sst_body = sst_scratch + SST_HEADER_LEN;
        sst_offset = 0;
        sst_walk_bad = false;
        sys_latch_t was;
        was.state = sst_scratch[16];
        was.held = (sst_scratch[17] & 1) != 0;
        was.breaking = (sst_scratch[17] & 2) != 0;
        sys_latch_apply(&was);
#define DRIVER(i, t, iot, r, s, b, c1, c2, sst) sst
#define SST(id, ver, size, sv, ld) sst_row_load(#id, (ver), (size), ld, flags);
        DRIVERS_FORWARD(RP6502_MACH_DRIVERS)
#undef SST
#undef DRIVER
        if (sst_walk_bad)
            return "the machine could not be put back";
    }
    return why;
}
