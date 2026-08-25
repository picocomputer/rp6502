/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_MON_UF2_H_
#define _RIA_MON_UF2_H_

/* Monitor command FLASH: self-update the RIA from a UF2 file on FatFs.
 */

#include <stdbool.h>

void uf2_task(void);
bool uf2_active(void);
void uf2_mon_flash(const char *args);

#endif /* _RIA_MON_UF2_H_ */
