/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The console again, into a file this side owns.
 *
 * The Pocket's own debug log stops writing while the core is still
 * running, and nothing this side does keeps it going -- which leaves a
 * restore unreadable, since that is where it stops. This is the same
 * bytes going somewhere the core owns instead.
 *
 * Off unless RP6502_LOG_FILE is defined, so it stays in the tree rather
 * than being added and removed around every question. An on build
 * spends one of the drive's eight descriptors and holds it, which costs
 * the two conformance cases that ask for all eight.
 *
 * The ring is the soft CPU's memory, which is the one thing a savestate
 * blob carries. That is the property this exists for: the drive defers
 * every operation while a restore is in flight, so the restore's own
 * lines cannot be written as they happen. They wait here and go out
 * afterwards, brought across by the blob with everything else.
 */

#include "log.h"

#ifdef RP6502_LOG_FILE

#include "com.h"
#include "main.h"
#include "mmio.h"
#include "msc.h"
#include "sst.h"

#include "core/api/api.h"
#include "core/api/std.h"

#include "host.h"

#include <stdint.h>

#define LOG_PATH "rp6502.log"

/* Power of two. Big enough to hold a restore's whole narration while the
 * drive is deferring, which is the longest this has to carry. */
#define LOG_RING_SIZE 4096
#define LOG_RING_MASK (LOG_RING_SIZE - 1)

/* The drive writes one bridge window at a time and a longer ask only
 * makes it say so; matching it keeps every write whole, which is what
 * lets the same chunk be handed back unchanged on a retry. */
#define LOG_CHUNK FILE_WIN_SIZE

static char log_ring[LOG_RING_SIZE];
static uint16_t log_head, log_tail;
static uint32_t log_dropped;
static int log_desc;
static uint32_t log_inflight;
static bool log_syncing;
static host_deadline_t log_retry_at;

void log_init(void)
{
    log_head = 0;
    log_tail = 0;
    log_dropped = 0;
    log_desc = -1;
    log_inflight = 0;
    log_syncing = false;
    log_retry_at = 0;
}

/* Dropped rather than overwritten: the bytes ahead of the head may be
 * the chunk currently at the bridge, and a count of what was lost is
 * worth more than a log that quietly disagrees with itself. */
void log_putc(char c)
{
    uint16_t next = (uint16_t)((log_head + 1) & LOG_RING_MASK);
    if (next == log_tail)
    {
        log_dropped++;
        return;
    }
    log_ring[log_head] = c;
    log_head = next;
}

void log_task(void)
{
    /* Draining the staging store is this firmware's job and the bridge
     * does not wait for it: a file operation begun while the host is
     * streaming starves that drain and the queue overflows. main.c
     * keeps its own boot line unsaid for the same reason, and a file
     * write is heavier than a console byte.
     *
     * Checked between operations rather than only before the first,
     * because a hot reload starts streaming into a machine that was
     * already running, and a flush underway then is the same
     * starvation. */
    if (!log_inflight && (!main_active() || sst_pending() || MMIO_SLOT))
        return;

    if (log_retry_at && !host_deadline_passed(log_retry_at))
        return;
    log_retry_at = 0;

    api_errno err = API_EIO;

    if (log_syncing)
    {
        if (msc_std_sync(log_desc, &err) == STD_PENDING)
            return;
        log_syncing = false;
        /* Said after the flush that carried the lines it counts, so the
         * number is never ahead of the gap it describes. */
        if (log_dropped)
        {
            uint32_t n = log_dropped;
            log_dropped = 0;
            com_printf("log: dropped %u\n", (unsigned)n);
        }
        return;
    }

    if (log_head == log_tail)
        return;

    if (log_desc < 0)
    {
        log_desc = msc_std_open(LOG_PATH,
                                MSC_O_WRITE | MSC_O_CREAT | MSC_O_APPEND,
                                &err);
        if (log_desc < 0)
        {
            /* A host still staging and a card that will not take the
             * file read the same from here, so this keeps asking rather
             * than deciding. */
            log_retry_at = host_deadline_ms(1000);
            return;
        }
    }

    /* Latched: msc_std_write copies into the bridge window on the call
     * that starts the operation and polls on the ones after, so the
     * count it is given has to be the same each time or the file
     * position advances by a length that was never written. */
    if (!log_inflight)
    {
        uint16_t run = log_head > log_tail
                           ? (uint16_t)(log_head - log_tail)
                           : (uint16_t)(LOG_RING_SIZE - log_tail);
        log_inflight = run > LOG_CHUNK ? LOG_CHUNK : run;
    }

    uint32_t wrote = 0;
    std_rw_result res = msc_std_write(log_desc, &log_ring[log_tail],
                                      log_inflight, &wrote, &err);
    if (res == STD_PENDING && !wrote)
        return;
    log_tail = (uint16_t)((log_tail + wrote) & LOG_RING_MASK);
    log_inflight = 0;
    if (res == STD_ERROR)
    {
        /* The descriptor is dropped rather than kept: a restore rebinds
         * every slot by name and an error here is as likely to be that
         * as the card. Reopening is how it recovers, and the ring holds
         * what has not gone out. */
        log_desc = -1;
        log_retry_at = host_deadline_ms(1000);
        return;
    }
    /* Only at the end of what there was: a sync per chunk would put a
     * flush command on the bridge for every window. */
    if (log_head == log_tail)
        log_syncing = true;
}

#endif /* RP6502_LOG_FILE */
