/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The regs symbol, the xstack and its pointer are hardware behind the OS
 * window, placed by the linker script.
 */

#include "core/mem.h"

volatile uint8_t *const xram = (uint8_t *)0x30000000u;

/* Link-time code generation emits calls to these after it has decided
 * what to keep, so they must outlive their callers. */
__attribute__((used)) void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

__attribute__((used)) void *memset(void *dst, int value, size_t n)
{
    uint8_t *d = dst;
    while (n--)
        *d++ = (uint8_t)value;
    return dst;
}

/* CRC-32/ISO-HDLC a nibble at a time: 64 bytes of table where the byte-wise
 * form wants 1 KB, and the loader that drives it is not this machine's inner
 * loop. Reflected polynomial, low nibble first: t[i] is 0xEDB88320 folded
 * through i four times. */
static const uint32_t mem_crc_nibble[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};


uint32_t mem_crc32(uint32_t crc, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len--)
    {
        crc ^= *p++;
        crc = (crc >> 4) ^ mem_crc_nibble[crc & 0x0F];
        crc = (crc >> 4) ^ mem_crc_nibble[crc & 0x0F];
    }
    return crc;
}
