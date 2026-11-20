/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_SW_SLEEP_H_
#define _HOST_POCKET_SW_SLEEP_H_

#include <stdbool.h>

/* A load is on its way or under way, and nothing else may start the
 * machine. */
bool sleep_waking(void);

/* Answers the host's save requests and readies its loads. main calls it
 * right after sys_commit, where the machine is settled as stopped or
 * running and no driver is partway through a pass. True when a load is
 * ready, which main then calls sleep_load for. */
bool sleep_task(void);

/* Answers the load sleep_task found ready, then makes it or refuses it. A
 * refused load leaves the machine stopped. A ROM the host loads once the
 * load is answered is a new one to start, so main takes the ROM the host
 * loaded for the wake as handled before it calls this. */
void sleep_load(void);

#endif /* _HOST_POCKET_SW_SLEEP_H_ */
