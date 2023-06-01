/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mem.h"
#include "littlefs/lfs_util.h"

char cbuf[CBUF_SIZE];

uint8_t mbuf[MBUF_SIZE] __attribute__((aligned(4)));
size_t mbuf_len;

// This CRC32 will match zlib
uint32_t mbuf_crc32()
{
    // use littlefs library
    return ~lfs_crc(~0, mbuf, mbuf_len);
}

// The xstack is:
// 256 bytes, enough to hold a CC65 stack frame
// 1 byte at end always zero for cstrings

// Many OS calls can use xstack instead of xram for cstrings.
// Using xstack doesn't require sending the zero termination.
// Cstrings and data are pushed in reverse so data is ordered correctly on a the top down stack.
// Cstrings and data are pulled in reverse to expedite use of returned y resister
// TODO can we use mbuf for this? it's big enough to reverse a 256 byte return.
uint8_t xstack[XSTACK_SIZE + 1];
size_t volatile xstack_ptr;

// 64KB Extended RAM
#ifdef NDEBUG
uint8_t xram[0x10000];
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
} xram_blocks;
uint8_t *const xram = (uint8_t *)&xram_blocks;
#endif
