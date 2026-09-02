/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * PHI2, the 6502's clock. Not a WDC part, but every WDC part runs on it, and
 * so does everything the 6502 shares a bus with.
 *
 * The range is here rather than with a machine because core/str/str.c
 * stringizes it into the SET help line and is compiled by every machine with
 * no machine directory on its include path.
 */

#ifndef _CORE_WDC_PHI2_H_
#define _CORE_WDC_PHI2_H_

#include <stdint.h>

#define PHI2_MIN_KHZ 100
#define PHI2_MAX_KHZ 8000
#define PHI2_DEFAULT_KHZ 8000

/* The rate actually running, which is not always the rate asked for: a
 * machine whose divider cannot land on a kilohertz reports the one it can.
 * The setter is the runtime path (ria_attr_get/set, the config apply); the
 * configured value is the generated phi2_get_khz/phi2_set_khz pair. */
uint16_t phi2_get_khz_run(void);
void phi2_set_khz_run(uint16_t phi2_khz);

#endif /* _CORE_WDC_PHI2_H_ */
