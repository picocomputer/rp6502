/*
 * Copyright (c) 2025 Rumbledethumps
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
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// this struct of 4KB segments is because
// a single 64KB array crashes my debugger
static struct
{
    uint8_t _0[0x1000];
    uint8_t _1[0x1000];
    uint8_t _2[0x1000];
    uint8_t _3[0x1000];
    uint8_t _4[0x1000];
    uint8_t _5[0x1000];
    uint8_t _6[0x1000];
    uint8_t _7[0x1000];
    uint8_t _8[0x1000];
    uint8_t _9[0x1000];
    uint8_t _A[0x1000];
    uint8_t _B[0x1000];
    uint8_t _C[0x1000];
    uint8_t _D[0x1000];
    uint8_t _E[0x1000];
    uint8_t _F[0x1000];
} xram_blocks;
uint8_t *const __uninitialized_ram(xram) __attribute__((aligned(4))) =
    (uint8_t *)&xram_blocks;

uint8_t xram_queue_page;
uint8_t xram_queue_head;
uint8_t xram_queue_tail;
uint8_t xram_queue[256][2];

uint8_t xstack[XSTACK_SIZE + 1];
size_t volatile xstack_ptr;

volatile uint8_t __uninitialized_ram(regs)[0x20]
    __attribute__((aligned(0x20)));

uint8_t mbuf[MBUF_SIZE] __attribute__((aligned(4)));
size_t mbuf_len;

char response_buf[RESPONSE_BUF_SIZE];

static mem_read_callback_t mem_callback;
static absolute_time_t mem_timer;
static uint32_t mem_timeout_ms;
static size_t mem_size;

void mem_task(void)
{
    if (!mem_callback)
        return;
    while (mem_callback)
    {
        int ch = stdio_getchar_timeout_us(0);
        if (ch == PICO_ERROR_TIMEOUT)
            break;
        mem_timer = make_timeout_time_ms(mem_timeout_ms);
        mbuf[mbuf_len] = ch;
        if (++mbuf_len == mem_size)
        {
            mem_read_callback_t cc = mem_callback;
            mem_callback = NULL;
            cc(false);
        }
    }
    if (mem_callback && mem_timeout_ms &&
        absolute_time_diff_us(get_absolute_time(), mem_timer) < 0)
    {
        mem_read_callback_t cc = mem_callback;
        mem_callback = NULL;
        cc(true);
    }
}

void mem_break(void)
{
    mem_callback = NULL;
}

void mem_read_mbuf(uint32_t timeout_ms, mem_read_callback_t callback, size_t size)
{
    assert(size <= MBUF_SIZE);
    mem_size = size;
    mbuf_len = 0;
    mem_timeout_ms = timeout_ms;
    mem_timer = make_timeout_time_ms(mem_timeout_ms);
    mem_callback = callback;
}
