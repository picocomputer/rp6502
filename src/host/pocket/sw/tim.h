/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_SW_TIM_H_
#define _HOST_POCKET_SW_TIM_H_

/* What core/tim/tim.h asks of a machine, for the Pocket: the clock the dock
 * gave at boot, and the one offset the menu sets for its zone. */

#include <stdint.h>

void tim_init(void);

void tim_set_tz_minutes(int32_t min);

/* Sets mtime to us, and moves the wall time's base with it. */
void tim_clock_set(uint64_t us);

#define TIM_DRIVER DRIVER(INIT(tim_init))

#endif /* _HOST_POCKET_SW_TIM_H_ */
