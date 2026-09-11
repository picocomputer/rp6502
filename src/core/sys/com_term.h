/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_SYS_COM_TERM_H_
#define _CORE_SYS_COM_TERM_H_

#include <stdbool.h>
#include <stddef.h>

/* A terminal query's answer (DSR/CPR/DA) enters the console's input as though
 * it had been typed, so whatever reads the console finds it in order with the
 * bytes around it. It is held whole until that input is empty and dropped
 * rather than truncated when it does not fit, so it can never land inside a
 * sequence still being delivered. A machine with a real terminal on the far
 * end, which answers the query itself, drops the reply instead. */
void com_in_write_reply(const char *s, size_t n);

void com_suppress_term_reply(bool suppress);

void com_set_term_out(void (*out_chars)(const char *buf, int len));

#endif /* _CORE_SYS_COM_TERM_H_ */
