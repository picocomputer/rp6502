/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The engine, with its trace hook reporting each write as it is applied. */

#include <stdint.h>

void ym_trace(uint8_t reg, uint8_t val);
#define YM3812_TRACE(reg, val) ym_trace((uint8_t)(reg), (uint8_t)(val))

#include "core/aud/ym3812.c"
