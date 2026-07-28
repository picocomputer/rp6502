/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The soft CPU's code memory, loaded the way the bitstream will load
 * it. The TCM is four byte-lane arrays because that is the only shape
 * the fabric will hold in block memory, so a firmware image spreads
 * across the lanes a byte at a time.
 */

#ifndef _TESTS_FPGA_TB_TCM_H_
#define _TESTS_FPGA_TB_TCM_H_

#include <cstdint>
#include <cstdio>

template <typename Lane>
static bool tb_load_tcm(Lane &t0, Lane &t1, Lane &t2, Lane &t3,
                        const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    for (size_t i = 0; i < 32768; i++)
        t0[i] = t1[i] = t2[i] = t3[i] = 0;
    uint8_t buf[4];
    size_t word = 0, n;
    while ((n = fread(buf, 1, 4, f)) > 0 && word < 32768)
    {
        t0[word] = n > 0 ? buf[0] : 0;
        t1[word] = n > 1 ? buf[1] : 0;
        t2[word] = n > 2 ? buf[2] : 0;
        t3[word] = n > 3 ? buf[3] : 0;
        word++;
    }
    fclose(f);
    return true;
}

#endif /* _TESTS_FPGA_TB_TCM_H_ */
