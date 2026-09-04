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

#include <stdbool.h>
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

/* Every machine brings up its own clock and answers the same configuration
 * row, so the row lives here rather than three times over. phi2_response is
 * only defined where a monitor asks for it; everywhere else the column falls
 * in the tail CONFIG_INT discards. */
void phi2_init(void);
bool phi2_check_khz(uint16_t *v);
void phi2_apply_khz(uint16_t phi2_khz, bool changed);

/* This driver's row in a machine's driver list; see core/sys/driver.h. */
#define PHI2_CONFIG_KHZ CONFIG_INT(P, phi2, khz, uint16_t, PHI2_DEFAULT_KHZ, \
    phi2_check_khz, phi2_apply_khz, STR_PHI2, phi2_response, STR_HELP_SET_PHI2, NULL)
#define PHI2_DRIVER DRIVER(phi2_init, nul_task, nul_task, nul_run, nul_stop, nul_break, \
    PHI2_CONFIG_KHZ, nul_config)

#endif /* _CORE_WDC_PHI2_H_ */
