/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_LOG_H_
#define _FPGA_SW_LOG_H_

void log_init(void);
void log_task(void);

/* Every console byte, from com_tx_write's fan-out. */
void log_putc(char c);

#endif /* _FPGA_SW_LOG_H_ */
