/*
 * Copyright (c) 2025 Rumbledethumps
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

#define AUD_L_PIN 28
#define AUD_R_PIN 27
#define AUD_PWM_IRQ_PIN 14 /* No IO */

/* Main events
 */

void aud_init(void);
void aud_stop(void);

/* Setup an audio system, tears down any previous setup.
 */

void aud_setup(void (*irq_fn)(void), uint32_t rate);

/* Audio sample depth and center
 */

#define AUD_PWM_BITS 10
#define AUD_PWM_CENTER (1u << (AUD_PWM_BITS - 1))

/* Audio drivers manage their own irq and sample rate.
 */

#define AUD_IRQ_SLICE (pwm_gpio_to_slice_num(AUD_PWM_IRQ_PIN))

/* Convenience for pwm_set_chan_level.
 */

#define AUD_L_CHAN (pwm_gpio_to_channel(AUD_L_PIN))
#define AUD_L_SLICE (pwm_gpio_to_slice_num(AUD_L_PIN))
#define AUD_R_CHAN (pwm_gpio_to_channel(AUD_R_PIN))
#define AUD_R_SLICE (pwm_gpio_to_slice_num(AUD_R_PIN))

#endif /* _RIA_AUD_AUD_H_ */
