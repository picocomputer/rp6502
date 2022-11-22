
/*
 * Copyright (c) 2022 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VRAM_H_
#define _VRAM_H_

#include <stdint.h>

// 64KB Virtual RAM
#ifdef NDEBUG
extern uint8_t vram[0xFFFF];
#else
extern uint8_t *const vram;
#endif

#endif /* _VRAM_H_ */
