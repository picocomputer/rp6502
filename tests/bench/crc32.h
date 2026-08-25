/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * CRC-32/ISO-HDLC, for suites that write down what a machine produced.
 *
 * The machine has one of these too — mem_crc32, which the 6502 can call — and
 * this is deliberately not that one. A test that checked a frame with the
 * implementation under test would be asking the machine to mark its own work;
 * test_units is where mem_crc32 answers for itself.
 *
 * Header-only and tiny, because both trees' suites want it and neither should
 * have to link a machine to get it.
 */

#ifndef _TESTS_BENCH_CRC32_H_
#define _TESTS_BENCH_CRC32_H_

#include <stddef.h>
#include <stdint.h>

static inline uint32_t bench_crc32(const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t crc = 0xFFFFFFFFu;
    while (len--)
    {
        crc ^= *p++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

#endif /* _TESTS_BENCH_CRC32_H_ */
