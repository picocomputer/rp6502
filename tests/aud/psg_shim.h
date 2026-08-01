/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TESTS_FPGA_PSG_SHIM_H_
#define _TESTS_FPGA_PSG_SHIM_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    void shim_init(void);
    void shim_sample(int16_t *l, int16_t *r);
    void shim_xram_write(uint16_t addr, uint8_t val);
    uint32_t shim_xram_word(uint16_t word_addr);

#ifdef __cplusplus
}
#endif

#endif /* _TESTS_FPGA_PSG_SHIM_H_ */
