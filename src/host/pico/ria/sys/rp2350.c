/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ria/sys/rp2350.h"
#include <hardware/clocks.h>
#include <hardware/vreg.h>

/* The boost SYS_RP2350_KHZ is tested at. Nothing else asks what voltage the
 * part is running on, so nothing else is told. */
#define SYS_RP2350_VREG VREG_VOLTAGE_1_15

void rp2350_init(void)
{
    vreg_set_voltage(SYS_RP2350_VREG);
    set_sys_clock_khz(SYS_RP2350_KHZ, true);
}
