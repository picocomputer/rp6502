/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The pins that have to be right before anything else runs: the 6502 held in
 * reset, and the bus conditioned for the speed the PIO programs will need.
 *
 * First in this machine's roster. Everything after it may print, allocate a
 * state machine, or take a round trip to the VGA -- none of which is safe
 * with a 6502 loose on the bus.
 *
 * The hook is gpio_pins_init and not gpio_init because the Pico SDK owns
 * that name, with a different signature.
 */

#ifndef _RIA_SYS_GPIO_H_
#define _RIA_SYS_GPIO_H_

void gpio_pins_init(void);

/* This driver's lifecycle row; see core/lifecycle.h. */
#define GPIO_LIFECYCLE LIFECYCLE(gpio_pins_init, nul_run, nul_stop, nul_break)

#endif /* _RIA_SYS_GPIO_H_ */
