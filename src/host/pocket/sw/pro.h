/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_PRO_H_
#define _FPGA_SW_PRO_H_

#include <stdint.h>

/* Ask what the staged image is called and make that argv, dropping any
 * exec still waiting: a program the user picked supersedes the one the
 * outgoing program asked for. Blocking, machine stopped. */
void pro_restage(void);

/* The image the machine is actually running, as the host spelled it.
 * After a restore this is the blob's answer, which is the program the
 * restored session belongs to -- not whatever the host has slot 0 bound
 * to now. Empty before anything has been staged. */
const char *pro_staged_path(void);

/* Record what the EXIT syscall returned, for the attribute that reports it. */
void pro_set_exit_code(int16_t code);

#endif /* _FPGA_SW_PRO_H_ */
