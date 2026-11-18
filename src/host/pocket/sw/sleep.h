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

/* Answers the host's save and load requests. main calls it right after
 * sys_commit, where the machine is settled as stopped or running and no
 * driver is partway through a pass. True when a load has just been made
 * or refused, which main then takes as its own: the ROM the host loaded
 * for the wake is not a new one to start. A refused load leaves the
 * machine stopped. */
bool sleep_task(void);

#endif /* _HOST_POCKET_SW_SLEEP_H_ */
