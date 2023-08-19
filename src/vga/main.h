/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _MAIN_H_
#define _MAIN_H_

#include <stdint.h>

void main_reclock(void);
void main_pix_cmd(uint8_t addr, uint16_t word);

#endif /* _MAIN_H_ */
