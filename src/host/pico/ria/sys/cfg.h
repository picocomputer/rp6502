/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_CFG_H_
#define _RIA_SYS_CFG_H_

/* System configuration manager.
 */

#include "core/cfg.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void cfg_init(void);

// The boot string isn't stored in RAM.
void cfg_save_boot(const char *str);
const char *cfg_load_boot(void);

/* This driver's row in a machine's driver list; see core/mach.h. After LFS, which holds
 * the file, and before every row that adopts a default -- each of those asks
 * whether the config already set one. */
#define CFG_DRIVER DRIVER(cfg_init, nul_task, nul_task, nul_run, nul_stop, nul_break)

#endif /* _RIA_SYS_CFG_H_ */
