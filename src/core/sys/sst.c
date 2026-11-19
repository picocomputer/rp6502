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
 * fixed for a build and a frontend that allocates once always allocates
 * enough.
 */

#include "core/sys/sst.h"

#include "core/api/std.h"
#include "core/aud/mix.h"
#include "core/sys/sys.h"
#include "core/wdc/resb.h"
#include "host/host.h"
#include "machine.h"
#include "drivers.h"

#include <string.h>

#define SST_MAGIC "RP65"
#define SST_END_MAGIC "56PR" /* the magic reversed, so a truncated tail is not a header */
#define SST_FORMAT 2

/* magic 4, format 2, count 2, total 4, manifest 4, latch 2 */
#define SST_HEADER_LEN 18
/* id 4, version 2, used 4 */
#define SST_CHUNK_HDR 10
/* payload sum 4, end magic 4 */
#define SST_TRAILER_LEN 8

/* Hands every driver's row to fn, in driver list order, until fn returns
 * false. A row is a compound literal inside the call, whose parentheses keep
 * its commas from splitting the driver list. */
static bool sst_each(bool (*fn)(const sst_row_t *r, void *ctx), void *ctx)
{
#define SST(id, name)                                  \
    if (!fn(&(const sst_row_t){id, &name##_sst}, ctx)) \
        return false;
#include "core/sys/driver_walk.h"
    DRIVERS_FORWARD(RP6502_MACH_DRIVERS)
#include "core/sys/driver_walk_end.h"
    return true;
}

/* MSVC defines neither macro. */
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
               "sst_word_t's first byte is its low byte");
#endif

static uint32_t sst_row_size(const sst_row_t *r)
{
    return r->table->size;
}

/* A chunk's version is the low half of its description's sum, so a chunk
 * from a build whose list differs is refused even where the manifest is not
 * checked first. */
static uint16_t sst_row_version(const sst_row_t *r)
{
    const char *d = r->table->desc;
    return (uint16_t)host_crc32(0, d, strlen(d));
}

static bool sst_add_size(const sst_row_t *r, void *ctx)
{
    *(size_t *)ctx += SST_CHUNK_HDR + sst_row_size(r);
    return true;
}

size_t sst_size(void)
{
    size_t total = SST_HEADER_LEN + SST_TRAILER_LEN;
    sst_each(sst_add_size, &total);
    return total;
}

static bool sst_add_one(const sst_row_t *r, void *ctx)
{
    (void)r;
    ++*(uint16_t *)ctx;
    return true;
}

static uint16_t sst_count(void)
{
    uint16_t count = 0;
    sst_each(sst_add_one, &count);
    return count;
}

/* Every driver's id, version and slot size, hashed in driver list order, so a
 * machine whose list differs in any of the three refuses a blob rather than
 * loading chunks into the wrong drivers. */
static uint32_t sst_manifest_crc;
static bool sst_manifest_done;

static bool sst_manifest_row(const sst_row_t *r, void *ctx)
{
    uint8_t row[10];
    memcpy(row, r->id, 4);
    const uint16_t version = sst_row_version(r);
    row[4] = (uint8_t)(version >> 8);
    row[5] = (uint8_t)version;
    sst_be32_write(row + 6, sst_row_size(r));
    uint32_t crc = host_crc32(*(uint32_t *)ctx, row, sizeof row);
    crc = host_crc32(crc, r->table->desc, strlen(r->table->desc));
    *(uint32_t *)ctx = crc;
    return true;
}

static uint32_t sst_manifest(void)
{
    if (!sst_manifest_done)
    {
        uint32_t crc = 0;
        sst_each(sst_manifest_row, &crc);
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
static const char *sst_why;
/* A row's refusal names the row: its four-letter id, then the reason. */
static char sst_why_row[64];

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

static bool sst_row_save(const sst_row_t *r, void *ctx)
{
    unsigned flags = *(const unsigned *)ctx;
    uint8_t *p = sst_body + sst_offset;
    memcpy(p, r->id, 4);
    uint8_t *payload = p + SST_CHUNK_HDR;
    const uint32_t size = sst_row_size(r);
    sst_cursor_t c = {payload, payload + size, false, NULL};
    r->table->save(&c, flags);
    if (!sst_ok(&c))
    {
        sst_why = "a driver could not say what it holds";
        return false;
    }
    uint32_t used = (uint32_t)(c.at - payload);
    const uint16_t version = sst_row_version(r);
    p[4] = (uint8_t)(version >> 8);
    p[5] = (uint8_t)version;
    sst_be32_write(p + 6, used);
    /* The rest of the slot is zeroed rather than left as it was found, so a
     * machine in the same state always writes the same bytes into a buffer
     * that has been written before. */
    memset(c.at, 0, size - used);
    sst_offset += SST_CHUNK_HDR + size;
    return true;
}

static bool sst_row_load(const sst_row_t *r, void *ctx)
{
    unsigned flags = *(const unsigned *)ctx;
    const uint8_t *p = sst_body + sst_offset;
    uint32_t used = sst_be32_read(p + 6);
    uint16_t got = (uint16_t)((p[4] << 8) | p[5]);
    /* The manifest sst_load matched is computed from the driver list and not
     * from these bytes, and the payload sum that covers them runs only when
     * flags is clear, so a corrupt SST_TRUSTED or SST_SHARED blob arrives here
     * with nothing having read them. */
    const uint32_t size = sst_row_size(r);
    if (memcmp(p, r->id, 4) != 0 || got != sst_row_version(r) || used > size)
    {
        sst_why = sst_row_why(r->id, "not the chunk the manifest promised");
        return false;
    }
    uint8_t *payload = (uint8_t *)p + SST_CHUNK_HDR;
    sst_cursor_t c = {payload, payload + used, false, NULL};
    if (!r->table->load(&c, flags) || !sst_ok(&c))
    {
        sst_why = sst_row_why(r->id, c.why ? c.why : "refused what it was handed");
        return false;
    }
    sst_offset += SST_CHUNK_HDR + size;
    return true;
}

/* Every row loaded from the blob at body, in order, stopping at the first
 * that refuses. The PCM: device is emptied first, so the close that the STD
 * row makes of it has nothing to wait for while machine time stands still. */
static bool sst_load_rows(uint8_t *body, unsigned flags)
{
    aud_pcm_clear();
    sst_body = body;
    sst_offset = 0;
    sst_why = NULL;
    aud_sst_hold(true);
    const bool loaded = sst_each(sst_row_load, &flags);
    aud_sst_free(loaded);
    return loaded;
}

#define SST_SST(X)                                      \
    X(SKIP, sst_manifest_crc, "the savestate's own working memory") \
    X(SKIP, sst_manifest_done)                          \
    X(SKIP, sst_body)                                   \
    X(SKIP, sst_offset)                                 \
    X(SKIP, sst_why)                                    \
    X(SKIP, sst_why_row)
SST_TABLE(sst, SST_SST)

static uint32_t sst_payload_crc(const uint8_t *buf)
{
    return host_crc32(0, buf, sst_size() - SST_TRAILER_LEN);
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

    size_t total = sst_size();
    memcpy(buf, SST_MAGIC, 4);
    buf[4] = 0;
    buf[5] = SST_FORMAT;
    uint16_t count = sst_count();
    buf[6] = (uint8_t)(count >> 8);
    buf[7] = (uint8_t)count;
    sst_be32_write(buf + 8, (uint32_t)total);
    sst_be32_write(buf + 12, sst_manifest());
    buf[16] = latch.state;
    buf[17] = (uint8_t)((latch.held ? 1 : 0) | (latch.breaking ? 2 : 0));

    sst_body = buf + SST_HEADER_LEN;
    sst_offset = 0;
    sst_why = NULL;
    aud_sst_hold(false);
    const bool saved = sst_each(sst_row_save, &flags);
    aud_sst_free(true);
    if (!saved)
        return sst_why;

    sst_be32_write(buf + total - SST_TRAILER_LEN, sst_payload_crc(buf));
    memcpy(buf + total - 4, SST_END_MAGIC, 4);
    return NULL;
}

const char *sst_save(void *buf, size_t len, unsigned flags)
{
    if (len < sst_size())
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
    if (total != sst_size() || len < total)
        return "a savestate of a different size";
    if (((uint16_t)(p[6] << 8 | p[7])) != sst_count())
        return "a savestate with a different chunk count";
    if (sst_be32_read(p + 12) != sst_manifest())
        return "a savestate from a build whose drivers differ";
    if (memcmp(p + total - 4, SST_END_MAGIC, 4) != 0)
        return "a savestate that does not end where it should";

    sys_latch_t latch;
    const char *why = sst_latch(p, &latch);
    if (why)
        return why;

    if (!flags &&
        sst_be32_read(p + total - SST_TRAILER_LEN) != sst_payload_crc(p))
        return "a savestate that does not add up";
    return NULL;
}

const char *sst_load(const void *buf, size_t len, unsigned flags)
{
    const char *why = sst_check(buf, len, flags);
    return why ? why : sst_load_checked(buf, flags);
}

const char *sst_load_checked(const void *buf, unsigned flags)
{
    const uint8_t *p = (const uint8_t *)buf;
    sys_latch_t latch;
    sst_latch(p, &latch);

    if (!sys_latch_apply(&latch))
        return "a savestate of a machine that could not have existed";

    if (sst_load_rows((uint8_t *)(uintptr_t)(p + SST_HEADER_LEN), flags))
        return NULL;

    sys_reset();
    return sst_why;
}
