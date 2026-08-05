/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What the host actually does with data slots, measured instead of
 * assumed. Built only when RP6502_POCKET_PROBE is on, and then the
 * machine loads no ROM: the screen is the instrument and a program would
 * scribble over it.
 *
 * WHAT THE FIRST PASS ESTABLISHED, on a Pocket. The table is indexed by
 * slot id — the pair for slot N is at words 2N and 2N+1, and ids 9 and
 * 10 carry 0xF000 and 0x13D8, the two assets' size_exact to the byte. So
 * the bench's model is right and pocket_bridge's word 17 really is the
 * ROM's size.
 *
 * WHAT IT DID NOT ESTABLISH, and what this pass is for. Changing the ROM
 * from the Core Settings menu always posts pocket_bridge's read of that
 * word, and sometimes also posts the update event. When the update is
 * absent the posted length was seen to be the PREVIOUS ROM's. That could
 * be the bridge reading the table before the host has finished writing
 * it, or the two events racing, or the first probe reporting its own
 * timing rather than the host's — the first pass printed the bridge's
 * number alone, with nothing beside it to judge it against.
 *
 * So this prints no interpretation. Per event, side by side: the length
 * pocket_bridge posted, the id and size read out of the table at that
 * instant, whether the update arrived and what it said, and then the
 * same table read once more a second later. If the table is stale when
 * the bridge looks and fresh when the machine does, the two reads
 * disagree and the second matches the ROM actually launched — which is
 * also why nothing has ever failed to launch: the firmware asks the
 * table, not the announcement.
 */

#include "mmio.h"
#include "msc.h"
#include "probe.h"

#include <stdio.h>

void probe_dump(void)
{
    printf("\nslot probe\n");
    for (uint32_t w = 0; w < 64; w += 4)
        printf("%03u %08x %08x %08x %08x\n", (unsigned)w,
               (unsigned)msc_dt(w), (unsigned)msc_dt(w + 1),
               (unsigned)msc_dt(w + 2), (unsigned)msc_dt(w + 3));
    /* The table is 256 words. Everything past the first sixty-four is
     * only worth a line if the host put something there. */
    for (uint32_t w = 64; w < 256; w++)
    {
        uint32_t v = msc_dt(w);
        if (v)
            printf("%03u %08x\n", (unsigned)w, (unsigned)v);
    }
    printf("set=%08x upd=%08x len=%08x\n", (unsigned)MMIO_SLOT,
           (unsigned)MMIO_UPD_ID, (unsigned)MMIO_UPD_LEN);
}

/* Word 16 and 17 are slot 8's pair, which the first pass measured. */
#define PROBE_DT_ID 16u
#define PROBE_DT_LEN 17u

static uint32_t probe_seq;
static uint64_t probe_settle_at;

static uint64_t probe_now(void)
{
    /* Read high, low, high: the low word wraps every 71 minutes and the
     * pair is not latched together. */
    for (;;)
    {
        uint32_t hi = MTIME_HI;
        uint32_t lo = MTIME_LO;
        if (hi == MTIME_HI)
            return ((uint64_t)hi << 32) | lo;
    }
}

void probe_task(void)
{
    if (probe_settle_at && probe_now() >= probe_settle_at)
    {
        probe_settle_at = 0;
        printf("  +1s  dt=%u/%u\n", (unsigned)msc_dt(PROBE_DT_ID),
               (unsigned)msc_dt(PROBE_DT_LEN));
    }

    /* Both registers taken before either is cleared, so an event landing
     * mid-print is not attributed to the wrong line. Bit 16 is the update
     * flag, because slot 0 is a real id. */
    uint32_t id = MMIO_UPD_ID;
    uint32_t set = MMIO_SLOT;
    if (!(id & 0x10000u) && !set)
        return;
    uint32_t upd_len = MMIO_UPD_LEN;
    MMIO_UPD_ID = 0;
    MMIO_SLOT = 0;

    /* The table as the machine would find it, asked now rather than
     * taken from the announcement. This is what a reload actually uses. */
    uint32_t dt_id = msc_dt(PROBE_DT_ID);
    uint32_t dt_len = msc_dt(PROBE_DT_LEN);

    printf("%u set=%u dt=%u/%u upd=%c%u/%u\n", (unsigned)++probe_seq,
           (unsigned)set, (unsigned)dt_id, (unsigned)dt_len,
           (id & 0x10000u) ? 'y' : 'n', (unsigned)(id & 0xFFFFu),
           (unsigned)upd_len);

    probe_settle_at = probe_now() + 1000000u;
}
