/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _MAIN_H_
#define _MAIN_H_

#include <stdbool.h>
#include <stdint.h>

void main_flush(void);
void main_reclock(void);
bool main_prog(uint16_t *xregs);

#endif /* _MAIN_H_ */
