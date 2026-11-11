/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The debug port, MMIO_CONSOLE: the port behind the debug pin and the
 * Pocket's debug log, where its bytes arrive in target command 0x0152 events.
 * host_log writes the machine's own log lines to it. */

#ifndef _HOST_POCKET_SW_LOG_H_
#define _HOST_POCKET_SW_LOG_H_

/* Connects to the debug port whatever goes there besides the log lines. */
void log_init(void);

#define LOG_DRIVER DRIVER(INIT(log_init))

#endif /* _HOST_POCKET_SW_LOG_H_ */
