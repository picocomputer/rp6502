/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The seam between a console and the terminal it carries -- core/term/term.c
 * on one side, whatever answers core/sys/com.h on the other.
 *
 * Its own header because the two rosters are not the same one. A RIA links no
 * term.c and can never answer these; a VGA chip has no program and answers
 * nothing in com.h. Kept together they made a contract no machine implements
 * in full, and every implementer looked half finished. */

#ifndef _CORE_SYS_COM_TERM_H_
#define _CORE_SYS_COM_TERM_H_

#include <stdbool.h>
#include <stddef.h>

/* A terminal query's answer (DSR/CPR/DA), entering the console's input as
 * though it had been typed -- as the UART source, since the program asked for
 * it and is waiting. Dropped rather than truncated if it does not fit, and
 * held until the wire it shares that source with is empty, so it can never
 * land inside a sequence still being delivered. */
void com_in_write_reply(const char *s, size_t n);

/* Stop answering those queries, because something at the far end of the wire
 * is a real terminal and will answer them itself. Two terminals answering one
 * query is one answer too many. Wiring rather than machine state: a cold boot
 * does not touch it. */
void com_suppress_term_reply(bool suppress);

/* The sink term.c hands over at init; the console fans printf output to it
 * alongside its own. */
void com_set_term_out(void (*out_chars)(const char *buf, int len));

#endif /* _CORE_SYS_COM_TERM_H_ */
