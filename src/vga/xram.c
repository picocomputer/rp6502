/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "xram.h"

#ifdef NDEBUG
uint8_t xram[0x10000]
    __attribute__((aligned(0x10000)))
    __attribute__((section(".uninitialized_data.xram")));
#else
struct
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
} xram_blocks
    __attribute__((aligned(0x10000)))
    __attribute__((section(".uninitialized_data.xram")));
uint8_t *const xram = (uint8_t *)&xram_blocks;
#endif
