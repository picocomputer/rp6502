/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * A machine made of nothing, so that the order of the walks is the only thing
 * left to see. Three rows, each column of each row writing its own two letters
 * into a log the test reads back. Real rows do work; these only say they ran.
 *
 * Two of the three carry a chunk and the third does not, which is what lets
 * the savestate walks be seen the same way: that a row with nothing to say
 * contributes no bytes at all is as much a claim as the order of the rest.
 *
 * core/sys/sys.c finds this by bare name, the way every machine's roster is
 * found, because the test names this directory and links no other machine.
 */

#ifndef _HOST_DRIVERS_H_
#define _HOST_DRIVERS_H_

#include "core/sys/driver.h"
#include "core/sys/sst.h"

void a_init(void), a_task(void), a_io(void), a_run(void), a_stop(void), a_break(void);
void b_init(void), b_task(void), b_io(void), b_run(void), b_stop(void), b_break(void);
void c_init(void), c_task(void), c_io(void), c_run(void), c_stop(void), c_break(void);

/* Two sizes that are not each other's, so a row handed the wrong slot reads
 * a boundary rather than a plausible number. */
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
