/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The one pin that has to be right before anything else runs: the 6502 held
 * in reset. No reset reaches the fabric's register, so from power-up the
 * 6502 is loose until this pulls it down -- which is why it is the first row
 * and not wherever the shared order would have put the CPU.
 *
 * Named gpio_pins_init to match the RIA's row, whose name is forced by the
 * Pico SDK owning gpio_init.
 */

#ifndef _FPGA_SW_GPIO_H_
#define _FPGA_SW_GPIO_H_

void gpio_pins_init(void);

/* This driver's lifecycle row; see core/lifecycle.h. */
#define GPIO_LIFECYCLE LIFECYCLE(gpio_pins_init, nul_run, nul_stop, nul_break)

#endif /* _FPGA_SW_GPIO_H_ */
