/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_SW_CFG_H_
#define _HOST_POCKET_SW_CFG_H_

void cfg_task(void);

#define CFG_DRIVER DRIVER(TASK_PUMPED(cfg_task))

#endif /* _HOST_POCKET_SW_CFG_H_ */
