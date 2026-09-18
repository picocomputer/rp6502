/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_SW_APF_H_
#define _HOST_POCKET_SW_APF_H_

void apf_init(void);
void apf_task(void);

void apf_refresh(void);

#define APF_DRIVER DRIVER(apf_init, apf_task, nul_task, nul_run, nul_stop, nul_break, nul_config, nul_config, nul_sst)

#endif /* _HOST_POCKET_SW_APF_H_ */
