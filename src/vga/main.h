/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VGA_MAIN_H_
#define _VGA_MAIN_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

void main_flush(void);
void main_reclock(void);
bool main_prog(uint16_t *xregs);

#endif /* _VGA_MAIN_H_ */
