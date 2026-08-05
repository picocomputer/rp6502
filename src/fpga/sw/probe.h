/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_PROBE_H_
#define _FPGA_SW_PROBE_H_

/* The data table as the host laid it out, once, at boot. */
void probe_dump(void);

/* One line per slot event, in the task order. */
void probe_task(void);

#endif /* _FPGA_SW_PROBE_H_ */
