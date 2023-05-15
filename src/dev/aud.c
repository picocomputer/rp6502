/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "aud.h"

#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/timer.h"
#include "hardware/irq.h"
#include <math.h>
#include <stdio.h>

#define AUD_L_PIN 28
#define AUD_L_CHAN PWM_CHAN_A
#define AUD_L_SLICE (pwm_gpio_to_slice_num(AUD_L_PIN))
#define AUD_R_PIN 27
#define AUD_R_CHAN PWM_CHAN_B
#define AUD_R_SLICE (pwm_gpio_to_slice_num(AUD_R_PIN))

#define AUD_IRQ_PIN 14
#define AUD_IRQ_SLICE (pwm_gpio_to_slice_num(AUD_IRQ_PIN))

#define AUD_PWM_WRAP 255
#define AUD_RATE 12000

// Fixed point range of -1.999 to 1.999 for DSP work
typedef signed short s1x14;
#define muls1x14(a, b) ((s1x14)((((int)(a)) * ((int)(b))) >> 14))
#define divs1x14(a, b) ((s1x14)((((signed int)(a) << 14) / (b))))
#define int_to_s1x14(a) ((s1x14)((a) << 14))
#define float_to_s1x14(a) ((s1x14)((a)*16384.0))
#define s1x14_to_float(a) ((float)(a) / 16384.0)

static s1x14 nco_r, nco_i;
static s1x14 clk_r, clk_i;

static void __isr __time_critical_func(audio_pwm_dma_irq_handler)()
{
    pwm_clear_irq(AUD_IRQ_SLICE);

    s1x14 r = ((int)nco_r * (int)clk_r - (int)nco_i * (int)clk_i) >> 14;
    s1x14 i = ((int)nco_i * (int)clk_r + (int)nco_r * (int)clk_i) >> 14;

    if (r < 0)
        pwm_set_chan_level(AUD_L_SLICE, AUD_L_CHAN, 0x80 - (-r >> 7));
    else
        pwm_set_chan_level(AUD_L_SLICE, AUD_L_CHAN, 0x80 + (r >> 7));

    static unsigned norm = 1;
    if (--norm)
    {
        nco_r = r;
        nco_i = i;
    }
    else
    {
        norm = 0x7F;
        s1x14 gain = int_to_s1x14(2) - (((int)r * (int)r + (int)i * (int)i) >> 14);
        nco_r = muls1x14(r, gain);
        nco_i = muls1x14(i, gain);
    }
}

void aud_init()
{
    gpio_set_function(AUD_L_PIN, GPIO_FUNC_PWM);
    gpio_set_function(AUD_R_PIN, GPIO_FUNC_PWM);

    pwm_config config;

    config = pwm_get_default_config();
    pwm_config_set_wrap(&config, AUD_PWM_WRAP);
    pwm_init(AUD_L_SLICE, &config, true);
    pwm_set_chan_level(AUD_L_SLICE, AUD_L_CHAN, AUD_PWM_WRAP / 2);

    config = pwm_get_default_config();
    pwm_config_set_wrap(&config, 240000u / (AUD_RATE / 1000));
    pwm_init(AUD_IRQ_SLICE, &config, true);

    nco_r = int_to_s1x14(1);
    nco_i = int_to_s1x14(0);

    float freq = 440.0; // C4
    // freq = 4186.009; // C8
    // freq = 2093.005; // C7

    float inc = M_PI * 2 * freq / AUD_RATE;
    clk_r = float_to_s1x14(cosf(inc));
    clk_i = float_to_s1x14(sinf(inc));

    pwm_set_irq_enabled(AUD_IRQ_SLICE, true);
    irq_set_exclusive_handler(PWM_IRQ_WRAP, audio_pwm_dma_irq_handler);
    irq_set_enabled(PWM_IRQ_WRAP, true);
}
