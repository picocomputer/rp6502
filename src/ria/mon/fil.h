
/*
 * Copyright (c) 2025 Rumbledethumps
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

/* Monitor commands
 */

void fil_mon_chdir(const char *args, size_t len);
void fil_mon_mkdir(const char *args, size_t len);
void fil_mon_chdrive(const char *args, size_t len);
void fil_mon_ls(const char *args, size_t len);
void fil_mon_upload(const char *args, size_t len);
void fil_mon_unlink(const char *args, size_t len);

#endif /* _RIA_MON_FIL_H_ */
