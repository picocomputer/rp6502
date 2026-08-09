/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_PRO_H_
#define _FPGA_SW_PRO_H_

/* Ask what the staged image is called and make that argv, dropping any
 * exec still waiting: a program the user picked supersedes the one the
 * outgoing program asked for. Blocking, machine stopped. */
void pro_restage(void);

#endif /* _FPGA_SW_PRO_H_ */
