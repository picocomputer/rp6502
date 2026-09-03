/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * CRC-32/ISO-HDLC a nibble at a time: 64 bytes of table where the byte-wise
 * form wants 1 KB, and nothing that drives it is a machine's inner loop.
 * Reflected polynomial, low nibble first: t[i] is 0xEDB88320 folded through
 * i four times. A host that already links one -- littlefs's, on the RIA --
 * answers host.h with that instead of compiling this.
 */

#include "host/host.h"

static const uint32_t crc32_nibble[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

uint32_t host_crc32(uint32_t crc, const void *buf, size_t len)
{
    crc ^= 0xFFFFFFFFu;
    const uint8_t *p = buf;
    while (len--)
    {
        crc ^= *p++;
        crc = (crc >> 4) ^ crc32_nibble[crc & 0x0F];
        crc = (crc >> 4) ^ crc32_nibble[crc & 0x0F];
    }
    return crc ^ 0xFFFFFFFFu;
}
