/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* sst_save and sst_load walk this machine's roster, turning it into a blob
 * and back.
 *
 * Every chunk has a slot of the size its driver declares, at an offset the
 * roster order decides, and a row that writes less than its slot is zero
 * padded to the end of it. Nothing is variable, so the total is a compile
 * time constant and a frontend that allocates once always allocates enough.
 */

#include "core/sys/sst.h"

#include "core/api/std.h"
#include "core/sys/sys.h"
#include "core/wdc/resb.h"
#include "host/host.h"

/* A row's slot size may be built from the machine's own constants, so
 * machine.h comes before the roster in drivers.h. */
#include "machine.h"
#include "drivers.h"

#include <string.h>

#define SST_MAGIC "RP65"
#define SST_END_MAGIC "56PR" /* the magic reversed, so a truncated tail is not a header */
#define SST_FORMAT 1

/* magic 4, format 2, count 2, total 4, manifest 4, latch 2 */
#define SST_HEADER_LEN 18
/* id 4, version 2, used 4 */
#define SST_CHUNK_HDR 10
/* payload sum 4, end magic 4 */
#define SST_TRAILER_LEN 8

/* The DRIVER expansions in this file take nine parameters where every other
 * consumer of a row is variadic. This is the only file that binds the ninth
 * column, so a row written with eight would compile everywhere else and
 * contribute no chunk; fixed arity makes it an error here instead. */
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

/* A load overwrites the machine row by row, so a row that refuses partway
 * through leaves the rows before it already applied. This holds the machine
 * as it stood when the walk began. It is a static rather than an allocation
 * because sst_load fills it before the first row is written and replays it
 * after a refusal, and neither may fail for want of memory. */
static uint8_t sst_scratch[SST_TOTAL];

/* Every row's id, version and slot size, hashed in roster order, so a machine
 * whose roster differs in any of the three refuses a blob rather than walk it
 * into the wrong rows. */
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
        /* The stdio table's length goes in too, because the STD chunk writes
         * each open descriptor's driver as an index into that table. */
        size_t std_count;
        (void)std_drivers(&std_count);
        uint8_t n = (uint8_t)std_count;
        crc = host_crc32(crc, &n, 1);
        sst_manifest_crc = crc;
        sst_manifest_done = true;
    }
    return sst_manifest_crc;
}

static uint8_t *sst_body;
static size_t sst_offset;
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
    /* The rest of the slot is zeroed rather than left as it was found, so a
     * machine in the same state always writes the same bytes into a buffer
     * that has been written before. */
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
    /* The manifest sst_load matched is computed from the roster and not from
     * these bytes, and the payload sum that covers them runs only when flags
     * is clear, so a corrupt SST_TRUSTED or SST_SHARED blob arrives here with
     * nothing having read them. */
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

static uint32_t sst_payload_crc(const uint8_t *buf)
{
    return host_crc32(0, buf, SST_TOTAL - SST_TRAILER_LEN);
}

static const char *sst_write(uint8_t *buf, unsigned flags)
{
    sys_latch_t latch;
    sys_latch_get(&latch);
    /* Only stopped (0) and running (2) are written, because starting and
     * stopping are requests the next sys_commit has yet to perform and
     * sys_latch_apply refuses to restore either. A save between proc_boot's
     * sys_run and that commit sees starting. */
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
    (void)rom;
    const uint8_t *p = (const uint8_t *)buf;

    if (len < SST_HEADER_LEN)
        return "too short to be a savestate";
    if (memcmp(p, SST_MAGIC, 4) != 0)
        return "not a savestate";
    if (p[4] != 0 || p[5] != SST_FORMAT)
        return "a savestate format this build does not know";
    /* The size is taken from the blob and never from len, because a frontend
     * hands over the length it recorded when the file was written, which may
     * be another build's. */
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

    if (!flags && sst_get32(p + SST_TOTAL - SST_TRAILER_LEN) != sst_payload_crc(p))
        return "a savestate that does not add up";

    bool guarded = !(flags & SST_TRUSTED);
    if (guarded && sst_write(sst_scratch, flags) != NULL)
        return "the machine could not be copied aside";

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
