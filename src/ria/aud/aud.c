/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "pico.h"
#include "main.h"
#include "aud/aud.h"
#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include <math.h>
#include <string.h>

static void (*aud_reclock_fn)(uint32_t sys_clk_khz);
static void (*aud_task_fn)();

static void aud_nop()
{
}

void aud_init(void)
{
    gpio_set_function(AUD_L_PIN, GPIO_FUNC_PWM);
    gpio_set_function(AUD_R_PIN, GPIO_FUNC_PWM);

    pwm_config config;

    config = pwm_get_default_config();
    pwm_config_set_wrap(&config, AUD_PWM_WRAP);
    pwm_init(AUD_L_SLICE, &config, true);
    pwm_init(AUD_R_SLICE, &config, true);
    pwm_set_chan_level(AUD_L_SLICE, AUD_L_CHAN, AUD_PWM_CENTER);
    pwm_set_chan_level(AUD_R_SLICE, AUD_R_CHAN, AUD_PWM_CENTER);

    config = pwm_get_default_config();
    pwm_init(AUD_IRQ_SLICE, &config, true);

    aud_stop();
}

void aud_stop(void)
{
    pwm_set_irq_enabled(AUD_IRQ_SLICE, false);
    irq_set_enabled(PWM_IRQ_WRAP, false);
    aud_reclock_fn = aud_nop;
    aud_task_fn = aud_nop;
    pwm_set_chan_level(AUD_L_SLICE, AUD_L_CHAN, AUD_PWM_CENTER);
    pwm_set_chan_level(AUD_R_SLICE, AUD_R_CHAN, AUD_PWM_CENTER);
}

void aud_reclock(uint32_t sys_clk_khz)
{
    aud_reclock_fn(sys_clk_khz);
}

void aud_task(void)
{
    aud_task_fn();
}

void aud_setup(
    void (*start_fn)(void),
    void (*reclock_fn)(uint32_t sys_clk_khz),
    void (*task_fn)(void))
{
    if (reclock_fn != aud_reclock_fn)
    {
        aud_stop();
        start_fn();
        reclock_fn(clock_get_hz(clk_sys) / 1000);
        pwm_set_irq_enabled(AUD_IRQ_SLICE, true);
        irq_set_enabled(PWM_IRQ_WRAP, true);
        aud_reclock_fn = reclock_fn;
        aud_task_fn = task_fn;
    }
}
