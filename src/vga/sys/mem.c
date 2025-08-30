/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mem.h"

#ifdef NDEBUG
volatile const uint8_t xram[0x10000] __attribute__((aligned(0x10000)));
#else
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
    // this struct of 4KB segments is because
    // a single 64KB array crashes my debugger
} xram_blocks __attribute__((aligned(0x10000)));
uint8_t volatile *const xram = (uint8_t *)&xram_blocks;
#endif
