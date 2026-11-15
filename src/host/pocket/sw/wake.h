/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_SW_WAKE_H_
#define _HOST_POCKET_SW_WAKE_H_

#include <stdbool.h>

/* A wake is in progress: a blob or a load command has come, a restore
 * has not been ended yet, or the console still shows why a wake did not
 * resume. */
bool wake_pending(void);

void wake_task(void);

#define WAKE_DRIVER DRIVER(TASK_FATFS(wake_task))

#endif /* _HOST_POCKET_SW_WAKE_H_ */
