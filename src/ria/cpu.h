/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CPU_H_
#define _CPU_H_

#include <stdbool.h>

void cpu_run();
void cpu_stop();
void cpu_init();
void cpu_task();
bool cpu_is_active();

#endif /* _CPU_H_ */
