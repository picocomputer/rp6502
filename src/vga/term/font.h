/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FONT_H_
#define _FONT_H_

#include <stdint.h>

extern uint8_t font8[2048];
extern uint8_t font16[4096];

void font_init(void);
void font_set_codepage(uint16_t cp);

#endif /* _FONT_H_ */
