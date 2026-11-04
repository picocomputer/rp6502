/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The YM3812 per-sample spec as plain code: slow, written for reading, and
 * independent of the engine. It renders a schedule of applied writes. */

#ifndef _TESTS_HOST_EMU_OPL_MODEL_H_
#define _TESTS_HOST_EMU_OPL_MODEL_H_

#include <stdint.h>

void model_reset(void);
/* Applies a write before the next sample. */
void model_write(uint8_t reg, uint8_t val);
/* Renders one chip sample and returns its YM3014B word. */
int16_t model_sample(void);

#endif /* _TESTS_HOST_EMU_OPL_MODEL_H_ */
