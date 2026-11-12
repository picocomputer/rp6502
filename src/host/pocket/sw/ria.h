/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* core/sys/ria.h on this machine, and the RIA's UART registers: the bytes the
 * 6502 writes to TX go out on the console, and a console byte is staged in RX
 * when the 6502 asks for one. */

#ifndef _HOST_POCKET_SW_RIA_H_
#define _HOST_POCKET_SW_RIA_H_

void ria_task(void);

#define RIA_DRIVER DRIVER(TASK_PUMPED(ria_task))

#endif /* _HOST_POCKET_SW_RIA_H_ */
