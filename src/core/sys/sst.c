/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* sst_save and sst_load visit every driver in this machine's driver list,
 * turning the machine into a blob and back.
 *
 * Every chunk has a slot of the size its driver declares, at an offset the
 * order of the driver list decides, and a driver that writes less than its
 * slot is zero padded to the end of it. Nothing is variable, so the total is
 * a compile time constant and a frontend that allocates once always
 * allocates enough.
 */

#include "core/sys/sst.h"
#include "core/sys/sst_size.h"

#include "core/api/std.h"
#include "core/aud/mix.h"
#include "core/sys/sys.h"
#include "core/wdc/resb.h"
#include "host/host.h"

#include <string.h>

#define SST_MAGIC "RP65"
#define SST_END_MAGIC "56PR" /* the magic reversed, so a truncated tail is not a header */
#define SST_FORMAT 1

/* MSVC defines neither macro. */
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
               "sst_word_t's first byte is its low byte");
#endif

size_t sst_size(void)
{
    return SST_TOTAL;
}

/* Every driver's id, version and slot size, hashed in driver list order, so a
 * machine whose list differs in any of the three refuses a blob rather than
 * loading chunks into the wrong drivers. */
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
#define SST(id, ver, size, sv, ld)                              \
    _Static_assert(sizeof #id == 5, "a chunk id is four bytes"); \
    crc = sst_manifest_step(crc, #id, (ver), (size));
#include "core/sys/driver_walk.h"
        DRIVERS_FORWARD(RP6502_MACH_DRIVERS)
#include "core/sys/driver_walk_end.h"
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
/* A row's refusal names the row: its four-letter id, then the reason. */
static char sst_why_row[64];
/* The reason the row being loaded gave, or NULL. */
static const char *sst_refusal;

bool sst_refuse(sst_cursor_t *c, const char *why)
{
    c->fail = true;
    sst_refusal = why;
    return false;
}

static const char *sst_row_why(const char *id, const char *reason)
{
    memcpy(sst_why_row, id, 4);
    sst_why_row[4] = ':';
    sst_why_row[5] = ' ';
    size_t n = strlen(reason);
    if (n > sizeof sst_why_row - 7)
        n = sizeof sst_why_row - 7;
    memcpy(sst_why_row + 6, reason, n);
    sst_why_row[6 + n] = '\0';
    return sst_why_row;
}

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
    sst_be32_write(p + 6, used);
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
    uint32_t used = sst_be32_read(p + 6);
    uint16_t got = (uint16_t)((p[4] << 8) | p[5]);
    /* The manifest sst_load matched is computed from the driver list and not
     * from these bytes, and the payload sum that covers them runs only when
     * flags is clear, so a corrupt SST_TRUSTED or SST_SHARED blob arrives here
     * with nothing having read them. */
    if (memcmp(p, id, 4) != 0 || got != ver || used > size)
    {
        sst_walk_bad = true;
        sst_why = sst_row_why(id, "not the chunk the manifest promised");
        return;
    }
    uint8_t *payload = (uint8_t *)p + SST_CHUNK_HDR;
    sst_cursor_t c = {payload, payload + used, false};
    sst_refusal = NULL;
    if (!load(&c, flags) || !sst_ok(&c))
    {
        sst_walk_bad = true;
        sst_why = sst_row_why(id, sst_refusal ? sst_refusal
                                              : "refused what it was handed");
        return;
    }
    sst_offset += SST_CHUNK_HDR + size;
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
    sst_be32_write(buf + 8, SST_TOTAL);
    sst_be32_write(buf + 12, sst_manifest());
    buf[16] = latch.state;
    buf[17] = (uint8_t)((latch.held ? 1 : 0) | (latch.breaking ? 2 : 0));

    aud_sst_settle();
    sst_body = buf + SST_HEADER_LEN;
    sst_offset = 0;
    sst_walk_bad = false;
    sst_why = NULL;
#define SST(id, ver, size, sv, ld) sst_row_save(#id, (ver), (size), sv, flags);
#include "core/sys/driver_walk.h"
    DRIVERS_FORWARD(RP6502_MACH_DRIVERS)
#include "core/sys/driver_walk_end.h"
    if (sst_walk_bad)
        return sst_why;

    sst_be32_write(buf + SST_TOTAL - SST_TRAILER_LEN, sst_payload_crc(buf));
    memcpy(buf + SST_TOTAL - 4, SST_END_MAGIC, 4);
    return NULL;
}

const char *sst_save(void *buf, size_t len, unsigned flags)
{
    if (len < SST_TOTAL)
        return "the buffer is too small";
    return sst_write((uint8_t *)buf, flags);
}

/* The blob's latch, which a valid blob holds. */
static const char *sst_latch(const uint8_t *p, sys_latch_t *latch)
{
    latch->state = p[16];
    latch->held = (p[17] & 1) != 0;
    latch->breaking = (p[17] & 2) != 0;
    if ((p[17] & ~(uint8_t)3) || (latch->state != 0 && latch->state != 2) ||
        (latch->state == 0 && !latch->held))
        return "a savestate of a machine that could not have existed";
    return NULL;
}

const char *sst_check(const void *buf, size_t len, unsigned flags)
{
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
    uint32_t total = sst_be32_read(p + 8);
    if (total != SST_TOTAL || len < total)
        return "a savestate of a different size";
    if (((uint32_t)p[6] << 8 | p[7]) != SST_COUNT)
        return "a savestate with a different chunk count";
    if (sst_be32_read(p + 12) != sst_manifest())
        return "a savestate from a build whose drivers differ";
    if (memcmp(p + SST_TOTAL - 4, SST_END_MAGIC, 4) != 0)
        return "a savestate that does not end where it should";

    sys_latch_t latch;
    const char *why = sst_latch(p, &latch);
    if (why)
        return why;

    if (!flags &&
        sst_be32_read(p + SST_TOTAL - SST_TRAILER_LEN) != sst_payload_crc(p))
        return "a savestate that does not add up";
    return NULL;
}

const char *sst_load(const void *buf, size_t len, void *undo, unsigned flags)
{
    const char *why = sst_check(buf, len, flags);
    return why ? why : sst_load_checked(buf, undo, flags);
}

const char *sst_load_checked(const void *buf, void *undo, unsigned flags)
{
    const uint8_t *p = (const uint8_t *)buf;
    sys_latch_t latch;
    sst_latch(p, &latch);

    if (undo && sst_write((uint8_t *)undo, flags) != NULL)
        return "the machine could not be copied aside";

    if (!sys_latch_apply(&latch))
        return "a savestate of a machine that could not have existed";

    /* The PCM: device is emptied first, so the close that the STD row makes
     * of it has nothing to wait for while machine time stands still. */
    aud_pcm_clear();
    sst_body = (uint8_t *)(uintptr_t)(p + SST_HEADER_LEN);
    sst_offset = 0;
    sst_walk_bad = false;
    sst_why = NULL;
#define SST(id, ver, size, sv, ld) sst_row_load(#id, (ver), (size), ld, flags);
#include "core/sys/driver_walk.h"
    DRIVERS_FORWARD(RP6502_MACH_DRIVERS)
#include "core/sys/driver_walk_end.h"
    if (!sst_walk_bad)
        return NULL;

    const char *why = sst_why;
    if (undo)
    {
        const uint8_t *u = (const uint8_t *)undo;
        aud_pcm_clear();
        sst_body = (uint8_t *)undo + SST_HEADER_LEN;
        sst_offset = 0;
        sst_walk_bad = false;
        sys_latch_t was;
        was.state = u[16];
        was.held = (u[17] & 1) != 0;
        was.breaking = (u[17] & 2) != 0;
        sys_latch_apply(&was);
#define SST(id, ver, size, sv, ld) sst_row_load(#id, (ver), (size), ld, flags);
#include "core/sys/driver_walk.h"
        DRIVERS_FORWARD(RP6502_MACH_DRIVERS)
#include "core/sys/driver_walk_end.h"
        if (sst_walk_bad)
            return "the machine could not be put back";
    }
    return why;
}
