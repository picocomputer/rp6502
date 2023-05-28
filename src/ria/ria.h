/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_H_
#define _RIA_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "hardware/pio.h"

// Content of these 15 pins is bound to the PIO program structure.
#define RIA_PIN_BASE 6
#define RIA_CS_PIN (RIA_PIN_BASE + 0)
#define RIA_RWB_PIN (RIA_PIN_BASE + 1)
#define RIA_DATA_PIN_BASE (RIA_PIN_BASE + 2)
#define RIA_ADDR_PIN_BASE (RIA_PIN_BASE + 10)
#define RIA_PHI2_PIN 21

void ria_init();
void ria_task();
void ria_reclock(uint16_t clkdiv_int, uint8_t clkdiv_frac);

#endif /* _RIA_H_ */
