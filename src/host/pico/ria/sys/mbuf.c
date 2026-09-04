/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ria/sys/mbuf.h"
#include "ria/sys/com.h"
#include <pico.h>
#include <pico/time.h>
#include <assert.h>
#include <stdalign.h>

alignas(4) uint8_t mbuf[MBUF_SIZE];
size_t mbuf_len;

static mbuf_read_callback_t mbuf_callback;
static absolute_time_t mbuf_deadline;
static uint32_t mbuf_timeout_ms;
static size_t mbuf_read_size;

void mbuf_task(void)
{
    while (mbuf_callback)
    {
        com_source_t src = COM_SOURCE_ANY;
        int c = com_getchar(&src);
        if (c < 0)
            break;
        mbuf_deadline = make_timeout_time_ms(mbuf_timeout_ms);
        mbuf[mbuf_len] = (uint8_t)c;
        if (++mbuf_len == mbuf_read_size)
        {
            mbuf_read_callback_t callback = mbuf_callback;
            mbuf_callback = NULL;
            callback(false);
            return;
        }
    }
    if (mbuf_callback && time_reached(mbuf_deadline))
    {
        mbuf_read_callback_t callback = mbuf_callback;
        mbuf_callback = NULL;
        callback(true);
    }
}

void mbuf_break(void)
{
    mbuf_callback = NULL;
}

void mbuf_read(uint32_t timeout_ms, mbuf_read_callback_t callback, size_t size)
{
    assert(!mbuf_callback);
    assert(timeout_ms);
    assert(size > 0 && size <= MBUF_SIZE);
    mbuf_read_size = size;
    mbuf_len = 0;
    mbuf_timeout_ms = timeout_ms;
    mbuf_deadline = make_timeout_time_ms(mbuf_timeout_ms);
    mbuf_callback = callback;
}
