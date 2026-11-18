/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_SW_MAIN_H_
#define _HOST_POCKET_SW_MAIN_H_

#include <stdint.h>

void tim_set_tz_minutes(int32_t min);

/* Sets mtime to us, and moves the wall time's base with it. */
void tim_clock_set(uint64_t us);

#endif /* _HOST_POCKET_SW_MAIN_H_ */
