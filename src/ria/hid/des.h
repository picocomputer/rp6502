/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _DES_H_
#define _DES_H_

#include <stdint.h>

uint32_t des_extract_bits(const uint8_t *report, uint16_t report_len, uint16_t bit_offset, uint8_t bit_size);
int32_t des_extract_signed(const uint8_t *report, uint16_t report_len, uint16_t bit_offset, uint8_t bit_size);
uint8_t des_scale_analog(uint32_t raw_value, uint8_t bit_size, int32_t logical_min, int32_t logical_max);
int8_t des_scale_analog_signed(uint32_t raw_value, uint8_t bit_size, int32_t logical_min, int32_t logical_max);

#endif // _DES_H_
