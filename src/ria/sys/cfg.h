/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_CFG_H_
#define _RIA_SYS_CFG_H_

/* System configuration manager.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void cfg_init(void);

// Unconditionally save the config.
void cfg_save(void);

// The boot string isn't stored in RAM.
void cfg_save_boot(const char *str);
const char *cfg_load_boot(void);

#endif /* _RIA_SYS_CFG_H_ */
