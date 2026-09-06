/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The host's stdio as this machine's console: the wire's two ends, and the
 * terminal on the far side of it when there is one.
 */

#ifndef _HOST_SOKOL_CLI_CONSOLE_H_
#define _HOST_SOKOL_CLI_CONSOLE_H_

#include <stdbool.h>

/* Put the host's stdio on the machine's console wire. True when the far end
 * is a terminal, which then is the console: keys raw, the machine's screen
 * drawn on it, and the emulated terminal left a mirror that answers nothing.
 * A pipe or a file only feeds the wire. */
bool console_open(void);

/* A run with nothing left to do: the program is parked on a read and the
 * wire is empty, so only the host can move it. Without this an unpaced run
 * spins the machine's clock forward over an empty wire. */
void console_idle(void);

#endif /* _HOST_SOKOL_CLI_CONSOLE_H_ */
