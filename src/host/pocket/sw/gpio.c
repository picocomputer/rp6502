/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * See gpio.h.
 */

#include "gpio.h"
#include "mmio.h"

void gpio_pins_init(void)
{
    CPU_RESB = 0;
}
