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

#define A_DRIVER DRIVER(                              \
    INIT(a_init)                                      \
    TASK_PUMPED(a_task)                               \
    TASK_FATFS(a_io)                                  \
    RUN(a_run)                                        \
    STOP(a_stop)                                      \
    BREAK(a_break)                                    \
    SST(AAAA, 1, A_SST_SIZE, a_sst_save, a_sst_load))
#define B_DRIVER DRIVER( \
    INIT(b_init)         \
    TASK_PUMPED(b_task)  \
    TASK_FATFS(b_io)     \
    RUN(b_run)           \
    STOP(b_stop)         \
    BREAK(b_break))
#define C_DRIVER DRIVER(                              \
    INIT(c_init)                                      \
    TASK_PUMPED(c_task)                               \
    TASK_FATFS(c_io)                                  \
    RUN(c_run)                                        \
    STOP(c_stop)                                      \
    BREAK(c_break)                                    \
    SST(CCCC, 2, C_SST_SIZE, c_sst_save, c_sst_load))

#define RP6502_MACH_DRIVERS A_DRIVER, B_DRIVER, C_DRIVER

#endif /* _HOST_DRIVERS_H_ */
