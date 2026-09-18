/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_SW_CFG_H_
#define _HOST_POCKET_SW_CFG_H_

void cfg_task(void);

#define CFG_DRIVER DRIVER(nul_init, cfg_task, nul_task, nul_run, nul_stop, nul_break, nul_config, nul_config, nul_sst)

#endif /* _HOST_POCKET_SW_CFG_H_ */
