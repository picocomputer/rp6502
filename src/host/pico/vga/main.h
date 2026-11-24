/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VGA_MAIN_H_
#define _VGA_MAIN_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

void main_pre_reclock(void);
void main_post_reclock(void);
bool main_prog(uint16_t *xregs);

/* The VGA is still at work on what the RIA sent it: a job of the console's
 * terminal is pending (TERM_JOB_ROWS), or a new view waits for its switch
 * (vga_switching). The reply to a message waits for this (ria_ack). */
bool main_busy(void);

#endif /* _VGA_MAIN_H_ */
