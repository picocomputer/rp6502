/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_COM_H_
#define _FPGA_SW_COM_H_

#include "core/com.h"
#include "ria/sys/com.h"

/* The wire to the terminal: term.c's captured stdio driver, registered
 * through the pico/stdio/driver.h shim. */
void com_set_term_out(void (*out_chars)(const char *buf, int len));

#endif /* _FPGA_SW_COM_H_ */
