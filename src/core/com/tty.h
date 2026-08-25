/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The console's wire, which is the only part of a console that is a machine's
 * own. Everything above it -- the rings, what a Ctrl-C means, when a BEL
 * rings, how a newline is spelled -- is core/com/com.c, shared.
 *
 * A machine whose console is a real serial port, with a FIFO of its own and a
 * reader that must choose between sources, does not fit above this seam and
 * keeps its own com.c. */

#ifndef _CORE_COM_TTY_H_
#define _CORE_COM_TTY_H_

#include <stdbool.h>
#include <stdint.h>

/* Terminal-bound bytes, already CRLF-translated. Where they go is the
 * machine's: a memory-mapped console register, a host's stderr. */
void tty_write(const char *buf, int len);

/* Take back a byte the register window staged ahead of a reader, if this
 * machine stages one. False when there is nothing to reclaim. */
bool tty_reg_reclaim(char *out);

#endif /* _CORE_COM_TTY_H_ */
