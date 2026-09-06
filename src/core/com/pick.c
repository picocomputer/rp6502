/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Which source the next console byte comes from. A machine lists its sources
 * in drivers.h -- a keyboard, a wire, a remote -- each a row of function
 * pointers over a queue that stays where it is; this file reads the rows.
 *
 * The rules here are the ones every console with more than one source needs.
 * A read holds the source it started on until that source has been dry for
 * its dwell, so a keystroke cannot cut into a paste and a burst on a wire is
 * not sliced by a byte from another. A byte the machine's register window
 * staged ahead of a reader comes back through the row it was taken from,
 * before that row is read, so no row can forget it and no path produces a
 * byte without its source.
 */

#include "core/sys/com.h"
#include "core/sys/timer.h"
#include "machine.h"
#include "drivers.h"

static HOST_IN_FLASH("com_sources") const com_source_driver_t
    com_sources[COM_SOURCE_COUNT] = {RP6502_COM_SOURCES};

/* The source a read is in the middle of, and how long it keeps the reader
 * once it has run dry. */
static com_source_t com_rx_held = COM_SOURCE_ANY;
static timer_deadline_t com_rx_deadline;

static size_t com_read_source(com_source_t s, char *buf, size_t length)
{
    size_t n = com_rx_reclaim(buf, length, s);
    if (n < length && com_sources[s].read)
        n += com_sources[s].read(&buf[n], length - n);
    return n;
}

/* The enum's order is the try order: keyboard, then wire, then remote. */
static size_t com_rx_pick(char *buf, size_t length, com_source_t *src_out)
{
    if (com_rx_held != COM_SOURCE_ANY && timer_passed(com_rx_deadline))
        com_rx_held = COM_SOURCE_ANY;
    for (com_source_t s = COM_SOURCE_KEYBOARD; s < COM_SOURCE_COUNT; s++)
    {
        if (com_rx_held != COM_SOURCE_ANY && com_rx_held != s)
            continue;
        size_t n = com_read_source(s, buf, length);
        if (n)
        {
            com_rx_held = s;
            com_rx_deadline = timer_in_us(com_sources[s].dwell_us);
            if (src_out)
                *src_out = s;
            return n;
        }
        /* A row without a dwell is one whose empty queue means the user
         * stopped typing, so it lets go now. A row with one keeps the reader
         * through the gap in a burst still arriving. */
        if (!com_sources[s].dwell_us)
            com_rx_held = COM_SOURCE_ANY;
    }
    return 0;
}

int com_getchar(com_source_t *src)
{
    char ch;
    size_t n = *src == COM_SOURCE_ANY ? com_rx_pick(&ch, 1, src)
                                      : com_read_source(*src, &ch, 1);
    if (n)
        return (unsigned char)ch;
    *src = COM_SOURCE_ANY;
    return -1;
}

int com_peekchar(com_source_t src)
{
    if (src >= COM_SOURCE_COUNT)
        return -1;
    int c = com_rx_peek(src);
    if (c >= 0)
        return c;
    return com_sources[src].peek ? com_sources[src].peek() : -1;
}

/* One source per read: what the hold promises to a reader that takes a
 * buffer at a time is the same as what it promises to one that takes a byte,
 * and a raw TTY: read never gets two sources spliced in one buffer. */
size_t com_stdin_read(char *buf, size_t count)
{
    return com_rx_pick(buf, count, NULL);
}

void com_rx_clear(void)
{
    com_rx_held = COM_SOURCE_ANY;
    for (com_source_t s = COM_SOURCE_KEYBOARD; s < COM_SOURCE_COUNT; s++)
        if (com_sources[s].clear)
            com_sources[s].clear();
}
