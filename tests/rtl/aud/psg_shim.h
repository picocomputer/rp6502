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

    void shim_xram_write(uint16_t addr, uint8_t val);

#ifdef __cplusplus
}
#endif

#endif /* _TESTS_AUD_PSG_SHIM_H_ */
