/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_SW_WAKE_H_
#define _HOST_POCKET_SW_WAKE_H_

#include <stdbool.h>

bool wake_pending(void);

void wake_task(void);

#define WAKE_DRIVER DRIVER(TASK_FATFS(wake_task))

#endif /* _HOST_POCKET_SW_WAKE_H_ */
