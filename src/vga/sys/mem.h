/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _MEM_H_
#define _MEM_H_

#include <stdint.h>

// 64KB Extended RAM
#ifdef NDEBUG
extern volatile const uint8_t xram[0x10000];
#else
extern volatile uint8_t *const xram;
#endif

#endif /* _MEM_H_ */
