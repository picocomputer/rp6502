/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TESTS_AUD_PSG_SHIM_H_
#define _TESTS_AUD_PSG_SHIM_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    void shim_init(void);
    void shim_sample(int16_t *l, int16_t *r);
    void shim_xram_write(uint16_t addr, uint8_t val);
    uint8_t shim_xram_read(uint16_t addr);

#ifdef __cplusplus
}
#endif

#endif /* _TESTS_AUD_PSG_SHIM_H_ */
