/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_SW_CFG_H_
#define _HOST_POCKET_SW_CFG_H_

/* Apply whatever the Pocket's menu has set since the last visit. */
void cfg_task(void);

/* This driver's row in a machine's driver list; see core/sys/driver.h. Polls the core's settings interface. */
#define CFG_DRIVER DRIVER(nul_init, cfg_task, nul_task, nul_run, nul_stop, nul_break, nul_config, nul_config)

#endif /* _HOST_POCKET_SW_CFG_H_ */
