/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The host's stdio as this machine's console.
 */

#ifndef _HOST_SOKOL_CLI_CONSOLE_H_
#define _HOST_SOKOL_CLI_CONSOLE_H_

#include <stdbool.h>

/* Connect the host's stdio to the machine's console. True when stdin and
 * stdout are the same terminal, which then takes the machine's screen and
 * answers the queries the emulated terminal would; a pipe or a file only
 * supplies input. */
bool console_open(void);

/* Called when the run has nothing else to do. A program blocked on a console
 * read with no input queued can only be moved by the host, so this waits up to
 * one frame for a byte rather than running the machine's clock forward over
 * nothing. The bound is what keeps a headless run pacing meanwhile. */
void console_idle(void);

#endif /* _HOST_SOKOL_CLI_CONSOLE_H_ */
