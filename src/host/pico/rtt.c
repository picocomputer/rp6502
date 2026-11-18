/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "rtt.h"
#include "machine.h"
#include <stdint.h>
#include <string.h>

#define RTT_UP_SIZE 256
#define RTT_DOWN_SIZE 64

/* A ring holds one byte less than its size, so that full and empty differ.
 * The chip moves wr of the up ring and rd of the down ring, and the debugger
 * moves the other two. Each offset has one writer, so nothing is locked.
 * The offsets are volatile because the debugger reads and writes them. */
typedef struct
{
    const char *name;
    char *buf;
    uint32_t size;
    volatile uint32_t wr;
    volatile uint32_t rd;
    uint32_t flags;
} rtt_up_t;

typedef struct
{
    const char *name;
    char *buf;
    uint32_t size;
    volatile uint32_t wr;
    volatile uint32_t rd;
    uint32_t flags;
} rtt_down_t;

static char rtt_up_buf[RTT_UP_SIZE];
static char rtt_down_buf[RTT_DOWN_SIZE];

/* The debugger finds this by its id, which is why the id is here in .data
 * and written nowhere else: an image that runs from RAM would otherwise hold
 * a second copy for the search to find. What follows the down ring is not
 * RTT's. It is ours, for a debugger that knows this firmware: `dropped`
 * counts the bytes rtt_write had no room for, and the debugger sets
 * `break_request` to ask for a break. */
static struct
{
    char id[16];
    int32_t up_count;
    int32_t down_count;
    rtt_up_t up;
    rtt_down_t down;
    volatile uint32_t dropped;
    volatile uint32_t break_request;
} rtt __attribute__((used)) = {
    .id = "SEGGER RTT",
    .up_count = 1,
    .down_count = 1,
    .up = {.name = "Terminal", .buf = rtt_up_buf, .size = RTT_UP_SIZE},
    .down = {.name = "Terminal", .buf = rtt_down_buf, .size = RTT_DOWN_SIZE},
};

void HOST_IN_RAM_FUNC rtt_write(const char *buf, size_t length)
{
    uint32_t wr = rtt.up.wr;
    uint32_t rd = rtt.up.rd;
    /* A debugger gone wrong must not send a write outside the ring. */
    if (rd >= RTT_UP_SIZE)
        rd = 0;
    uint32_t room = (rd + RTT_UP_SIZE - 1 - wr) % RTT_UP_SIZE;
    if (length > room)
    {
        rtt.dropped += (uint32_t)(length - room);
        length = room;
    }
    while (length--)
    {
        rtt_up_buf[wr] = *buf++;
        wr = (wr + 1) % RTT_UP_SIZE;
    }
    rtt.up.wr = wr;
}

static uint32_t HOST_IN_RAM_FUNC rtt_down_len(void)
{
    uint32_t wr = rtt.down.wr;
    if (wr >= RTT_DOWN_SIZE)
        return 0;
    return (wr + RTT_DOWN_SIZE - rtt.down.rd) % RTT_DOWN_SIZE;
}

size_t HOST_IN_RAM_FUNC rtt_read(char *buf, size_t length)
{
    uint32_t have = rtt_down_len();
    if (length > have)
        length = have;
    uint32_t rd = rtt.down.rd;
    for (size_t i = 0; i < length; i++)
    {
        buf[i] = rtt_down_buf[rd];
        rd = (rd + 1) % RTT_DOWN_SIZE;
    }
    rtt.down.rd = rd;
    return length;
}

bool HOST_IN_RAM_FUNC rtt_break(void)
{
    if (!rtt.break_request)
        return false;
    rtt.break_request = 0;
    return true;
}
