/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sys/mem.h"
#include <pico.h>

#if defined(DEBUG_RIA_SYS) || defined(DEBUG_RIA_SYS_MEM)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#ifdef NDEBUG
uint8_t xram[0x10000] __attribute__((aligned(4)));
#else
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
uint8_t *const xram __attribute__((aligned(4))) =
    (uint8_t *)&xram_blocks;
#endif

uint8_t xstack[XSTACK_SIZE + 1];
size_t volatile xstack_ptr;

volatile uint8_t __uninitialized_ram(regs)[0x20]
    __attribute__((aligned(0x20)));

uint8_t mbuf[MBUF_SIZE] __attribute__((aligned(4)));
size_t mbuf_len;

char response_buf[RESPONSE_BUF_SIZE];
