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

#include <stddef.h>

/* Terminal-bound bytes, already CRLF-translated. Where they go is the
 * machine's: a memory-mapped console port, or nowhere but a test's mirror. */
void tty_write(const char *buf, int len);

/* A host that puts a real wire on this machine's console installs both ends
 * here: terminal-bound bytes go out on tx, and what has arrived comes back
 * through rx, at most max bytes, as the UART source. Either may be NULL, and
 * both together are NULL for a machine whose console is the screen it
 * already renders. */
void tty_set_wire(void (*tx)(const char *buf, int len),
                  size_t (*rx)(char *buf, size_t max));

#endif /* _CORE_COM_TTY_H_ */
