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

#include "core/sys.h"
#include "log.h"

#ifdef RP6502_LOG_FILE

#include "com.h"
#include "main.h"
#include "mmio.h"
#include "fs.h"
#include "sst.h"

#include "core/api/api.h"
#include "core/api/std.h"

#include "host/host.h"

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
static host_deadline_t log_retry_at;

void log_init(void)
{
    log_head = 0;
    log_tail = 0;
    log_dropped = 0;
    log_desc = -1;
    log_inflight = 0;
    log_retry_at = 0;
}

static uint16_t log_room(void)
{
    return (uint16_t)((log_tail - log_head - 1) & LOG_RING_MASK);
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

/* A restore carries this descriptor's file position back with it, and
 * that position belongs to the session the blob was taken from. Left
 * alone, the woken machine resumes writing into the middle of the file
 * it is trying to record: the log becomes a composite of two runs
 * spliced at an offset neither of them chose, with whatever the two
 * did not cover left over from whoever held those sectors before. It
 * cost a hardware run to read one of those and believe it.
 *
 * So the binding is thrown away and taken again. FS_APPEND asks the
 * host how long the file is now, which is the one number that is true
 * for both sessions, and the record reads in order across the event
 * that made it. The ring is kept -- what is in it has not been written
 * yet and is the restored session's to say. */
void log_restore(void)
{
    if (log_desc >= 0)
        fs_release(log_desc);
    log_desc = -1;
    log_inflight = 0;
    log_retry_at = 0;
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
    if (!log_inflight && (!sys_active() || sst_pending() || MMIO_SLOT))
        return;

    if (log_retry_at && !host_deadline_passed(log_retry_at))
        return;
    log_retry_at = 0;

    api_errno err = API_EIO;

    /* Said as soon as the ring has room, not once it is empty: a
     * console the 6502 is flooding never empties, so that version was
     * silent through exactly the runs that dropped the most -- and a
     * log with unmarked gaps is worse than one that admits them. */
    if (log_dropped && log_room() > LOG_CHUNK)
    {
        uint32_t n = log_dropped;
        log_dropped = 0;
        com_printf("log: dropped %u\n", (unsigned)n);
    }

    if (log_head == log_tail)
        return;

    if (log_desc < 0)
    {
        log_desc = fs_std_open(LOG_PATH,
                                FS_WR | FS_CREAT | FS_APPEND,
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

    /* Latched: fs_std_write copies into the bridge window on the call
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
    std_rw_result res = fs_std_write(log_desc, &log_ring[log_tail],
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
    /* No sync. It was here, once the ring had emptied, on the belief
     * that a flush is what makes a write durable. 0x0188 draws no
     * response line from that console, ever, so it commits nothing, and
     * asking anyway cost 4391 commands against 16 writes in one
     * measured session. A write is durable when the host says it took
     * it. */
}

#endif /* RP6502_LOG_FILE */
