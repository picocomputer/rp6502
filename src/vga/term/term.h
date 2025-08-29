/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VGA_TERM_TERM_H_
#define _VGA_TERM_TERM_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void term_init(void);
void term_task(void);

bool term_prog(uint16_t *xregs);

#endif /* _VGA_TERM_TERM_H_ */
