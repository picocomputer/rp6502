/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_COM_H_
#define _FPGA_SW_COM_H_

#include "ria/sys/com.h"

/* Bytes moved through the manifold since boot, every direction summed.
 * The simulation's main loop watches it to know when the machine went
 * quiet; real hardware never exits and never asks. */
uint32_t com_moved(void);

/* The wire to the terminal: term.c's captured stdio driver, registered
 * through the pico/stdio/driver.h shim. */
void com_set_term_out(void (*out_chars)(const char *buf, int len));

/* Terminal query replies (DSR/CPR/DA) enter the merge as the UART source,
 * drained ahead of typed input. Declared by vga/sys/com.h for term.c; here
 * for the manifold's own callers. */
void com_in_write_reply(const char *s, size_t n);

#endif /* _FPGA_SW_COM_H_ */
