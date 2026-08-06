/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_PRO_H_
#define _FPGA_SW_PRO_H_

/* The host staged an image on the ROM slot: ask what it is called and
 * make that argv, and drop any exec still waiting — a program the user
 * picked supersedes the one the outgoing program asked for. Blocking,
 * and only ever called with the machine stopped. */
void pro_restage(void);

#endif /* _FPGA_SW_PRO_H_ */
