/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "aud/aud.h"
#include "sys/cpu.h"
#include <pico/stdlib.h>
#include <hardware/pwm.h>
#include <hardware/clocks.h>

#if defined(DEBUG_RIA_AUD) || defined(DEBUG_RIA_AUD_AUD)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

static irq_handler_t aud_irq_fn;

void aud_init(void)
{
    pwm_config config;

    config = pwm_get_default_config();
    pwm_config_set_wrap(&config, ((1u << AUD_PWM_BITS) - 1));
    pwm_init(AUD_L_SLICE, &config, true);
    pwm_init(AUD_R_SLICE, &config, true);

    config = pwm_get_default_config();
    pwm_init(AUD_IRQ_SLICE, &config, true);

    aud_stop();

    gpio_set_drive_strength(AUD_L_PIN, GPIO_DRIVE_STRENGTH_2MA);
    gpio_set_drive_strength(AUD_R_PIN, GPIO_DRIVE_STRENGTH_2MA);
    gpio_set_slew_rate(AUD_L_PIN, GPIO_SLEW_RATE_SLOW);
    gpio_set_slew_rate(AUD_R_PIN, GPIO_SLEW_RATE_SLOW);
    gpio_disable_pulls(AUD_L_PIN);
    gpio_disable_pulls(AUD_R_PIN);
    gpio_set_function(AUD_L_PIN, GPIO_FUNC_PWM);
    gpio_set_function(AUD_R_PIN, GPIO_FUNC_PWM);
}

void aud_stop(void)
{
    pwm_set_irq_enabled(AUD_IRQ_SLICE, false);
    irq_set_enabled(PWM_IRQ_WRAP, false);
    if (aud_irq_fn != NULL)
    {
        irq_remove_handler(PWM_IRQ_WRAP_0, aud_irq_fn);
        aud_irq_fn = NULL;
    }
    pwm_set_chan_level(AUD_L_SLICE, AUD_L_CHAN, AUD_PWM_CENTER);
    pwm_set_chan_level(AUD_R_SLICE, AUD_R_CHAN, AUD_PWM_CENTER);
}

void aud_setup(void (*irq_fn)(void), uint32_t rate)
{
    if (aud_irq_fn != irq_fn)
    {
        aud_stop();
        aud_irq_fn = irq_fn;
        irq_set_exclusive_handler(PWM_IRQ_WRAP_0, irq_fn);
        pwm_set_wrap(AUD_IRQ_SLICE, CPU_RP2350_KHZ / (rate / 1000.f));
        pwm_set_irq_enabled(AUD_IRQ_SLICE, true);
        irq_set_enabled(PWM_IRQ_WRAP, true);
    }
}
