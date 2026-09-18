/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_DRIVERS_H_
#define _HOST_DRIVERS_H_

#include "core/sys/driver.h"
#include "core/sys/sst.h"

void a_init(void), a_task(void), a_io(void), a_run(void), a_stop(void), a_break(void);
void b_init(void), b_task(void), b_io(void), b_run(void), b_stop(void), b_break(void);
void c_init(void), c_task(void), c_io(void), c_run(void), c_stop(void), c_break(void);

#define A_SST_SIZE 6
#define C_SST_SIZE 3
void a_sst_save(sst_cursor_t *c, unsigned flags);
bool a_sst_load(sst_cursor_t *c, unsigned flags);
void c_sst_save(sst_cursor_t *c, unsigned flags);
bool c_sst_load(sst_cursor_t *c, unsigned flags);

#define A_DRIVER DRIVER(a_init, a_task, a_io, a_run, a_stop, a_break, nul_config, nul_config, \
    SST(AAAA, 1, A_SST_SIZE, a_sst_save, a_sst_load))
#define B_DRIVER DRIVER(b_init, b_task, b_io, b_run, b_stop, b_break, nul_config, nul_config, nul_sst)
#define C_DRIVER DRIVER(c_init, c_task, c_io, c_run, c_stop, c_break, nul_config, nul_config, \
    SST(CCCC, 2, C_SST_SIZE, c_sst_save, c_sst_load))

#define RP6502_MACH_DRIVERS A_DRIVER, B_DRIVER, C_DRIVER

#endif /* _HOST_DRIVERS_H_ */
