/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The software machine's cold boot: one fan-out to every subsystem it has.
 * Everything else it answers for is elsewhere -- the run/stop lifecycle in
 * core/main.c, the syscall table in core/api/ops.c, the frame and bus engine
 * in core/sys/sys.c, and each chip's tick beside the chip.
 */

#ifndef _CORE_SYS_MAIN_H_
#define _CORE_SYS_MAIN_H_

#include "core/main.h"

#include <stdbool.h>
#include <stdint.h>
void main_init(void); /* cold boot: fan out to every subsystem */

#endif /* _CORE_SYS_MAIN_H_ */
