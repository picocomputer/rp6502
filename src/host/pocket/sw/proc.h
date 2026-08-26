/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_PROC_H_
#define _FPGA_SW_PROC_H_

#include <stdint.h>

/* Ask what the staged image is called and make that argv, dropping any
 * exec still waiting: a program the user picked supersedes the one the
 * outgoing program asked for. Blocking, machine stopped. */
void proc_restage(void);

/* The image the machine is actually running, as the host spelled it.
 * After a restore this is the blob's answer, which is the program the
 * restored session belongs to -- not whatever the host has slot 0 bound
 * to now. Empty before anything has been staged. */
const char *proc_staged_path(void);


#endif /* _FPGA_SW_PROC_H_ */
