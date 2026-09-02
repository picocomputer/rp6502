/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_SW_PHI2_H_
#define _HOST_POCKET_SW_PHI2_H_

#include "core/wdc/phi2.h"
#include <stdbool.h>

void phi2_init(void);

/* The configuration row's columns. */
bool phi2_check_khz(uint16_t *v);
void phi2_apply_khz(uint16_t phi2_khz, bool changed);

/* This driver's row in a machine's driver list; see core/driver.h. */
#define PHI2_CONFIG_KHZ CONFIG_INT(P, phi2, khz, uint16_t, PHI2_DEFAULT_KHZ, \
    phi2_check_khz, phi2_apply_khz, STR_PHI2, phi2_response, \
    STR_HELP_SET_PHI2, NULL)
#define PHI2_DRIVER DRIVER(phi2_init, nul_task, nul_task, nul_run, nul_stop, nul_break, \
    PHI2_CONFIG_KHZ, nul_config)

#endif /* _HOST_POCKET_SW_PHI2_H_ */
