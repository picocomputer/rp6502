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
 * stdout are the same terminal, which then takes the machine's screen; a pipe
 * or a file only supplies input. With a window, the window's terminal stays in
 * charge and answers queries. Without one, a terminal on stdio is in charge
 * and answers in place of the emulated one, and with a pipe or a file nothing
 * is in charge. */
bool console_open(bool window);

/* Called when the run has nothing else to do. A program blocked on a console
 * read with no input queued can only be moved by the host, so this waits up to
 * one frame for a byte rather than running the machine's clock forward over
 * nothing. The bound is what keeps a headless run pacing meanwhile. */
void console_idle(void);

#endif /* _HOST_SOKOL_CLI_CONSOLE_H_ */
