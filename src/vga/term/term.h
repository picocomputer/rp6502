/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TERM_H_
#define _TERM_H_

#include <stdbool.h>
#include <stdint.h>

void term_init(void);
void term_task(void);
bool term_prog(uint16_t *xregs);

#endif /* _TERM_H_ */
