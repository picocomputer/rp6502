/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mbuf.h"
#include "littlefs/lfs_util.h"

uint8_t mbuf[MBUF_SIZE] __attribute__((aligned(4)));
size_t mbuf_len;

// This CRC32 will match zlib
uint32_t mbuf_crc32()
{
    // use littlefs library
    return ~lfs_crc(~0, mbuf, mbuf_len);
}
