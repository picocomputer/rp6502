/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_MON_UF2_H_
#define _RIA_MON_UF2_H_

/* Monitor command FLASH: update the RIA or the VGA from a UF2 file on FatFs.
 */

#include <stdbool.h>

void uf2_task(void);
bool uf2_active(void);
void uf2_mon_flash(const char *args);

#define UF2_DRIVER DRIVER(nul_init, nul_task, uf2_task, nul_run, nul_stop, nul_break, nul_config, nul_config, nul_sst)

#endif /* _RIA_MON_UF2_H_ */
