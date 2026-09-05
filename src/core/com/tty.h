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
#include <stddef.h>
#include <stdint.h>

/* Terminal-bound bytes, already CRLF-translated. Where they go is the
 * machine's: a memory-mapped console port, or nowhere but a test's mirror. */
void tty_write(const char *buf, int len);

/* The program's stderr, raw. The terminal has already been given it; this
 * is for a machine with a stderr of its own. */
void tty_stderr_write(const char *buf, int len);

/* Take back a byte the register window staged ahead of a reader, if this
 * machine stages one. False when there is nothing to reclaim. */
bool tty_reg_reclaim(char *out);

/* A host that puts a real wire on this machine's console installs both ends
 * here: terminal-bound bytes go out on tx, and what has arrived comes back
 * through rx, at most max bytes, as the UART source. Either may be NULL, and
 * both together are NULL for a machine whose console is the screen it
 * already renders. */
void tty_set_wire(void (*tx)(const char *buf, int len),
                  size_t (*rx)(char *buf, size_t max));

/* Whether the program's stderr still gets its own copy on the host's. False
 * where the wire above already carries the terminal stream to the same
 * screen, which would otherwise show every error twice. */
void tty_set_stderr_host(bool on);

#endif /* _CORE_COM_TTY_H_ */
