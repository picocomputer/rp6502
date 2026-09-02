/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * PHI2 on a software machine, the same phase accumulator phi2_div.sv is: a
 * cycle is a whole number of system ticks plus a fraction of one, carried
 * from cycle to cycle, so every whole kilohertz in range is exact. The board
 * cannot do this -- a PIO clock divider is 16.8 fixed point and lands where
 * it lands -- and that difference is real hardware behaviour, not drift.
 */

#ifndef _CORE_WDC_PHI2_DIV_H_
#define _CORE_WDC_PHI2_DIV_H_

#include "core/wdc/phi2.h"
#include <stdbool.h>
#include <stdint.h>

void phi2_init(void);

/* The configuration row's columns. */
bool phi2_check_khz(uint16_t *v);
void phi2_apply_khz(uint16_t phi2_khz, bool changed);

/* The divider, hoisted into the bus loop once per call: a cycle is `whole`
 * system ticks, plus `frac` of `khz` accumulated toward one more. */
typedef struct
{
    uint32_t whole;
    uint32_t frac;
    uint32_t khz;
} phi2_div_t;

phi2_div_t phi2_div(void);

/* This driver's row in a machine's driver list; see core/driver.h. */
#define PHI2_CONFIG_KHZ CONFIG_INT(P, phi2, khz, uint16_t, PHI2_DEFAULT_KHZ, \
    phi2_check_khz, phi2_apply_khz, STR_PHI2, phi2_response, STR_HELP_SET_PHI2, NULL)
#define PHI2_DRIVER DRIVER(phi2_init, nul_task, nul_task, nul_run, nul_stop, nul_break, \
    PHI2_CONFIG_KHZ, nul_config)

#endif /* _CORE_WDC_PHI2_DIV_H_ */
