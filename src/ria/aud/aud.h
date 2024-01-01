/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _AUD_H_
#define _AUD_H_

#include "main.h"
#include <stdint.h>
#include <stdbool.h>

/* Kernel events
 */

void aud_init(void);
void aud_task(void);
void aud_stop(void);
void aud_reclock(uint32_t sys_clk_khz);

/* Setup an audio system, tears down previous if any
 */

void aud_setup(
    void (*start_fn)(void),
    void (*reclock_fn)(uint32_t sys_clk_khz),
    void (*task_fn)(void));


#define AUD_L_CHAN (pwm_gpio_to_channel(AUD_L_PIN))
#define AUD_L_SLICE (pwm_gpio_to_slice_num(AUD_L_PIN))
#define AUD_R_CHAN (pwm_gpio_to_channel(AUD_R_PIN))
#define AUD_R_SLICE (pwm_gpio_to_slice_num(AUD_R_PIN))
#define AUD_IRQ_SLICE (pwm_gpio_to_slice_num(AUD_PWM_IRQ_PIN))

#define AUD_BITS 8
#define AUD_PWM_WRAP ((1u << AUD_BITS) - 1)
#define AUD_PWM_CENTER (1u << (AUD_BITS - 1))
#define AUD_SHIFT (1 + 14 - AUD_BITS)


#endif /* _AUD_H_ */
