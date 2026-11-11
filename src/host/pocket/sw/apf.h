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

#define APF_DRIVER DRIVER(INIT(apf_init) TASK_PUMPED(apf_task))

#endif /* _HOST_POCKET_SW_APF_H_ */
