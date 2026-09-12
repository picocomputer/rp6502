/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/com.h"
#include "core/sys/timer.h"
#include "machine.h"
#include "drivers.h"

static const com_source_driver_t
    com_sources[COM_SOURCE_COUNT] = {RP6502_COM_SOURCES};

static com_source_t com_rx_held = COM_SOURCE_ANY;
static timer_mach_t com_rx_deadline;

static size_t com_read_source(com_source_t s, char *buf, size_t length)
{
    size_t n = com_rx_reclaim(buf, length, s);
    if (n < length && com_sources[s].read)
        n += com_sources[s].read(&buf[n], length - n);
    return n;
}

/* A source that read something holds the reader for the dwell it declares, so a
 * byte from another source cannot land between two chunks of a burst. A source
 * that declares no dwell is never held, because a deadline of zero microseconds
 * has already passed. */
static size_t com_rx_pick(char *buf, size_t length, com_source_t *src_out)
{
    if (com_rx_held != COM_SOURCE_ANY && timer_mach_passed(com_rx_deadline))
        com_rx_held = COM_SOURCE_ANY;
    for (com_source_t s = COM_SOURCE_KEYBOARD; s < COM_SOURCE_COUNT; s++)
    {
        if (com_rx_held != COM_SOURCE_ANY && com_rx_held != s)
            continue;
        size_t n = com_read_source(s, buf, length);
        if (n)
        {
            com_rx_held = s;
            com_rx_deadline = timer_mach_in_us(com_sources[s].dwell_us);
            if (src_out)
                *src_out = s;
            return n;
        }
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

size_t com_stdin_read(char *buf, size_t count)
{
    return com_rx_pick(buf, count, NULL);
}

void com_rx_save(sst_cursor_t *c)
{
    sst_put_u8(c, (uint8_t)com_rx_held);
    sst_put_u64(c, com_rx_deadline);
}

bool com_rx_load(sst_cursor_t *c)
{
    uint8_t held = sst_get_u8(c);
    uint64_t at = sst_get_u64(c);
    if (!sst_ok(c) || held > COM_SOURCE_ANY)
        return false;
    com_rx_held = (com_source_t)held;
    com_rx_deadline = at;
    return true;
}

void com_rx_clear(void)
{
    com_rx_held = COM_SOURCE_ANY;
    for (com_source_t s = COM_SOURCE_KEYBOARD; s < COM_SOURCE_COUNT; s++)
        if (com_sources[s].clear)
            com_sources[s].clear();
}
