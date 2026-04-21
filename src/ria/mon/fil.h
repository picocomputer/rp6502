
/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_MON_FIL_H_
#define _RIA_MON_FIL_H_

/* Monitor commands for filesystem.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void fil_task(void);
void fil_break(void);

// True when more work is pending.
bool fil_active(void);

// Predicts if a chdrive will succeed
bool fil_drive_exists(const char *args);

/* Monitor commands
 */

void fil_mon_chdir(const char *args);
void fil_mon_mkdir(const char *args);
void fil_mon_chdrive(const char *args);
void fil_mon_ls(const char *args);
void fil_mon_upload(const char *args);
void fil_mon_unlink(const char *args);

#endif /* _RIA_MON_FIL_H_ */
