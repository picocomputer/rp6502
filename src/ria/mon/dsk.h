/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_MON_DSK_H_
#define _RIA_MON_DSK_H_

/* Disk utility: format, zero, verify, partition, and label USB drives.
 */

#include <stdbool.h>

// Monitor command handler for "DISK".
void dsk_mon_disk(const char *args);

// Drives the long-running passes (format/zero/verify/part) across ticks.
void dsk_task(void);

// True while a confirm prompt or a pass is in progress.
bool dsk_active(void);

// Abort the current operation. A FORMAT UNIT in progress is not abortable and
// is left to complete.
void dsk_break(void);

#endif /* _RIA_MON_DSK_H_ */
