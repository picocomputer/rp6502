/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * A machine made of nothing, so that the order of the walks is the only thing
 * left to see. Three rows, each column of each row writing its own two letters
 * into a log the test reads back. Real rows do work; these only say they ran.
 *
 * core/sys/sys.c finds this by bare name, the way every machine's roster is
 * found, because the test names this directory and links no other machine.
 */

#ifndef _HOST_DRIVERS_H_
#define _HOST_DRIVERS_H_

#include "core/sys/driver.h"

void a_init(void), a_task(void), a_io(void), a_run(void), a_stop(void), a_break(void);
void b_init(void), b_task(void), b_io(void), b_run(void), b_stop(void), b_break(void);
void c_init(void), c_task(void), c_io(void), c_run(void), c_stop(void), c_break(void);

#define A_DRIVER DRIVER(a_init, a_task, a_io, a_run, a_stop, a_break, nul_config, nul_config)
#define B_DRIVER DRIVER(b_init, b_task, b_io, b_run, b_stop, b_break, nul_config, nul_config)
#define C_DRIVER DRIVER(c_init, c_task, c_io, c_run, c_stop, c_break, nul_config, nul_config)

#define RP6502_MACH_DRIVERS A_DRIVER, B_DRIVER, C_DRIVER

#endif /* _HOST_DRIVERS_H_ */
