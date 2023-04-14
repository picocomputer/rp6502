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
// These pins may be freely moved around but PHI2 on 21 is strongly
// recommended since no other pins support clock_gpio_init().
#define RIA_PHI2_PIN 21
#define RIA_RESB_PIN 28
#define RIA_IRQB_PIN 22
// Use both PIO blocks, constrained by address space
#define RIA_WRITE_PIO pio0
#define RIA_WRITE_SM 0
#define RIA_READ_PIO pio0
#define RIA_READ_SM 1
#define RIA_ACTION_PIO pio1
#define RIA_ACTION_SM 0
#define RIA_PIX_PIO pio1
#define RIA_PIX_SM 1

// Bit 28 always 1, bits [31:29] for payload type
#define RIA_PIX_XRAM 0x10000000u
#define RIA_PIX_XREG(channel) ((channel << 29u) | 0x10000000u)
#define RIA_PIX_IDLE 0xF0000000u

void ria_init();
void ria_task();
bool ria_is_active();
uint32_t ria_set_phi2_khz(uint32_t freq_khz);
uint32_t ria_get_reset_us();
void ria_stop();
void ria_reset();
void ria_exit();
bool ria_pix_ready();
void ria_pix_send(uint8_t ch3, uint16_t hi12, uint16_t lo16);

#endif /* _RIA_H_ */
