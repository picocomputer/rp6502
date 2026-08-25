/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The emulator's runner — the counterpart to ria/main.c. Cold boot (main_init),
 * the run/stop lifecycle, and the syscall op registry (main_api) the shared
 * core/api/api.c dispatches through via "main.h".
 *
 * The machine itself — the bus, the system clock and the frame engine — is
 * sys/sys.c; each chip's tick lives with the chip (sys/cpu.c, emu/via.c, sys/ria.c,
 * sys/mem.c).
 */

#ifndef _CORE_SYS_MAIN_H_
#define _CORE_SYS_MAIN_H_

#include "core/main.h"

#include <stdbool.h>
#include <stdint.h>


void main_init(void); /* cold boot: fan out to every subsystem */

/* Point the op table's dir slots at the firmware FatFs handlers (fat, over the
 * RAM disk) or the emu's host handlers. */
void main_dir_ops_set(bool fat);

#endif /* _CORE_SYS_MAIN_H_ */
