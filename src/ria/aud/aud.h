/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_AUD_AUD_H_
#define _RIA_AUD_AUD_H_

/* This audio manager allows for multiple audio
 * devices and ensures only one is active at any time.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void aud_init(void);
void aud_stop(void);

/* Setup an audio system, tears down any previous setup.
 */

void aud_setup(void (*irq_fn)(void), uint32_t rate);

/* Per-sample stereo output level and IRQ acknowledge, called from each audio
 * driver's sample handler. The firmware drives the PWM channels; the emulator
 * captures the levels.
 */

void aud_out(uint16_t left, uint16_t right);
void aud_clear_irq(void);

/* Audio sample depth and center
 */

#define AUD_PWM_BITS 10
#define AUD_PWM_CENTER (1u << (AUD_PWM_BITS - 1))

/* Sine table for waveform generation, shared by all audio drivers.
 */

extern int8_t aud_sine_table[256];

#endif /* _RIA_AUD_AUD_H_ */
