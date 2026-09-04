/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_AUD_PSG_H_
#define _CORE_AUD_PSG_H_

/* Programmable Sound Generator
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* One stereo sample at AUD_NATIVE_RATE, and the engine advanced by one. */
void psg_sample(int16_t *left, int16_t *right);

bool psg_xreg(uint16_t word);

#endif /* _CORE_AUD_PSG_H_ */
