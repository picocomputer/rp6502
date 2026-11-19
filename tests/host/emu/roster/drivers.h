/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_DRIVERS_H_
#define _HOST_DRIVERS_H_

#include "core/sys/driver.h"
#include "core/sys/sst.h"

void a_init(void), a_task(void), a_io(void), a_run(void), a_stop(void), a_break(void), a_reset(void);
void b_init(void), b_task(void), b_io(void), b_run(void), b_stop(void), b_break(void);
void b_always(void), a_flashing(void), c_flashing(void);
void c_init(void), c_task(void), c_io(void), c_run(void), c_stop(void), c_break(void), c_reset(void);

#define A_SST_SIZE 6
#define C_SST_SIZE 3
extern const sst_table_t a_sst, c_sst;

#define A_DRIVER DRIVER(                              \
    INIT(a_init)                                      \
    TASK_PUMPED(a_task)                               \
    TASK_FATFS(a_io)                                  \
    RUN(a_run)                                        \
    STOP(a_stop)                                      \
    BREAK(a_break)                                    \
    RESET(a_reset)                                    \
    FLASHING(a_flashing)                              \
    SST("AAAA", a))
#define B_DRIVER DRIVER( \
    INIT(b_init)         \
    TASK_PUMPED(b_task)  \
    TASK_ALWAYS(b_always) \
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
    RESET(c_reset)                                    \
    FLASHING(c_flashing)                              \
    SST("CCCC", c))

#define RP6502_MACH_DRIVERS A_DRIVER, B_DRIVER, C_DRIVER

#endif /* _HOST_DRIVERS_H_ */
