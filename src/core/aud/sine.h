/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_AUD_SINE_H_
#define _CORE_AUD_SINE_H_

#include <stdint.h>

/* The one table every voice reads. Sixteen bits: at eight it put a -49.9 dB
 * floor under every sine the PSG and the bell can make, which no amount of
 * width downstream could lift. */
extern int16_t sine_table[256];

/* Build it. Every machine calls this from its own aud_init. */
void sine_init(void);

#endif /* _CORE_AUD_SINE_H_ */
