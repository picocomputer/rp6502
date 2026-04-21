/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sys/mem.h"
#include <pico.h>
#include <pico/time.h>
#include <pico/stdio.h>
#include <assert.h>

#if defined(DEBUG_RIA_SYS) || defined(DEBUG_RIA_SYS_MEM)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// 4KB segments because a single 64KB array crashes my debugger
static uint8_t __uninitialized_ram(xram_blocks)[16][0x1000]
    __attribute__((aligned(4)));
uint8_t *const xram = (uint8_t *)xram_blocks;

volatile uint8_t xram_queue_page;
volatile uint8_t xram_queue_head;
volatile uint8_t xram_queue_tail;
volatile uint8_t xram_queue[256][2];

uint8_t xstack[XSTACK_SIZE + 1];
volatile size_t xstack_ptr;

volatile uint8_t __uninitialized_ram(regs)[0x20]
    __attribute__((aligned(0x20)));

uint8_t mbuf[MBUF_SIZE] __attribute__((aligned(4)));
size_t mbuf_len;

static mem_read_callback_t mem_callback;
static absolute_time_t mem_timer;
static uint32_t mem_timeout_ms;
static size_t mem_size;

void mem_task(void)
{
    while (mem_callback)
    {
        int ch = stdio_getchar_timeout_us(0);
        if (ch == PICO_ERROR_TIMEOUT)
            break;
        mem_timer = make_timeout_time_ms(mem_timeout_ms);
        mbuf[mbuf_len] = ch;
        if (++mbuf_len == mem_size)
        {
            mem_read_callback_t callback = mem_callback;
            mem_callback = NULL;
            callback(false);
        }
    }
    if (mem_callback && time_reached(mem_timer))
    {
        mem_read_callback_t callback = mem_callback;
        mem_callback = NULL;
        callback(true);
    }
}

void mem_break(void)
{
    mem_callback = NULL;
}

void mem_read_mbuf(uint32_t timeout_ms, mem_read_callback_t callback, size_t size)
{
    assert(!mem_callback);
    assert(timeout_ms);
    assert(size > 0 && size <= MBUF_SIZE);
    mem_size = size;
    mbuf_len = 0;
    mem_timeout_ms = timeout_ms;
    mem_timer = make_timeout_time_ms(mem_timeout_ms);
    mem_callback = callback;
}
