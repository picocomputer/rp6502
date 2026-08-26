/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_MON_DRIVE_H_
#define _RIA_MON_DRIVE_H_

/* Disk utility: show info, format, zero, verify, and label USB drives.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void drive_break(void);

// Monitor command handler for "DISK".
void drive_mon_disk(const char *args);

// True while a destructive/scan pass is running.
bool drive_active(void);

#endif /* _RIA_MON_DRIVE_H_ */
