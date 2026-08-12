/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The console again, into a file the core owns.
 *
 * The Pocket's debug log is the host's. It stops writing while the core
 * is still running -- three captures ended at or just after boot with
 * the machine alive and the menu working behind them -- and Analogue
 * describes it as a file written "as the boot process proceeds", so
 * nothing this side says after that has ever been read. Sitting behind
 * it is not a diagnosis, it is a closed pipe.
 *
 * This is the same bytes going somewhere else: the drive the machine
 * already has, through the same open and write a program's own file
 * uses. One more descriptor of the eight.
 *
 * The ring is the soft CPU's memory, which is the one thing a savestate
 * blob carries. That is the property this exists for: the drive defers
 * every operation while a restore is in flight -- msc_adrift -- so the
 * restore's own lines cannot be written as they happen. They wait here
 * and go out afterwards, brought across the reconfigure by the blob
 * along with everything else.
 */

#include "log.h"

#include "com.h"
#include "msc.h"

#include "ria/api/api.h"
#include "ria/api/std.h"

#include <pico/time.h>

#include <stdint.h>

#define LOG_PATH "rp6502.log"

/* Power of two. Big enough to hold a restore's whole narration while the
 * drive is deferring, which is the longest this ever has to carry. */
#define LOG_RING_SIZE 4096
#define LOG_RING_MASK (LOG_RING_SIZE - 1)

/* The drive writes one bridge window at a time and a longer ask only
 * makes it say so; matching it keeps every write whole, which is what
 * lets the same chunk be handed back unchanged on a retry. */
#define LOG_CHUNK 512

static char log_ring[LOG_RING_SIZE];
static uint16_t log_head, log_tail;
static uint32_t log_dropped;
static int log_desc;
static uint32_t log_inflight;
static bool log_syncing;
static absolute_time_t log_retry_at;

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
    if (log_retry_at && !time_reached(log_retry_at))
        return;
    log_retry_at = 0;

    api_errno err = API_EIO;

    if (log_syncing)
    {
        if (msc_std_sync(log_desc, &err) == STD_PENDING)
            return;
        log_syncing = false;
        /* Said after the flush that carried the lines it is counting, so
         * the number is never ahead of the gap it describes. */
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
            /* A host still staging, a restore the drive is deferring and
             * a card that will not take the file read the same from
             * here, so this keeps asking rather than deciding. */
            log_retry_at = make_timeout_time_ms(1000);
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
        /* The descriptor is dropped rather than kept: a wake rebinds
         * every slot by name and an error here is as likely to be that
         * as the card. Reopening is how it recovers, and the ring holds
         * what has not gone out. */
        log_desc = -1;
        log_retry_at = make_timeout_time_ms(1000);
        return;
    }
    /* Only at the end of what there was: a sync per chunk would put a
     * flush command on the bridge for every 512 bytes. */
    if (log_head == log_tail)
        log_syncing = true;
}
