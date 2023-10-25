/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "aud/aud.h"
#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include <math.h>
#include <string.h>

#define AUD_RATE 24000
#define AUD_BITS 8
#define AUD_CHANNELS 8

#define AUD_L_CHAN (pwm_gpio_to_channel(AUD_L_PIN))
#define AUD_L_SLICE (pwm_gpio_to_slice_num(AUD_L_PIN))
#define AUD_R_CHAN (pwm_gpio_to_channel(AUD_R_PIN))
#define AUD_R_SLICE (pwm_gpio_to_slice_num(AUD_R_PIN))
#define AUD_IRQ_SLICE (pwm_gpio_to_slice_num(AUD_PWM_IRQ_PIN))

#define AUD_PWM_WRAP ((1u << AUD_BITS) - 1)
#define AUD_PWM_CENTER (1u << (AUD_BITS - 1))
#define AUD_SHIFT (1 + 14 - AUD_BITS)

// Fixed point range of -1.9r to 1.9r for DSP work
typedef signed short s1x14;
#define muls1x14(a, b) ((s1x14)((((int)(a)) * ((int)(b))) >> 14))
#define float_to_s1x14(a) ((s1x14)((a) * 16384.f))
#define s1x14_to_float(a) ((float)(a) / 16384.f)
#define s1x14_0_0 (0)
#define s1x14_1_0 ((s1x14)(1 << 14))
#define s1x14_1_9r ((s1x14)(1 << 15) - 1)

enum waveform
{
    square,
    sine,
    // All above use sine, below is linear
    saw,
    triangle,
};

struct channel
{
    s1x14 nco_r;
    s1x14 nco_i;
    s1x14 clk_r;
    s1x14 clk_i;
    enum waveform wave;
    bool dirty;
};

// Recomputing clocks is too much for an ISR, so it's done as a task
// then moved from pending[] to chan[] as needed by the ISR.
static struct channel chan[AUD_CHANNELS];
static struct channel pending[AUD_CHANNELS];

static void __isr __time_critical_func(audio_pwm_irq_handler)()
{
    pwm_clear_irq(AUD_IRQ_SLICE);

    for (unsigned idx = 0; idx < AUD_CHANNELS; idx++)
    {
        struct channel *this = &chan[idx];
        if (pending[idx].dirty)
        {
            pending[idx].dirty = false;
            memcpy(this, &pending[idx], sizeof(struct channel));
        }

        enum waveform wave = this->wave;
        if (wave <= sine)
        {
            s1x14 r = ((int)this->nco_r * (int)this->clk_r - (int)this->nco_i * (int)this->clk_i) >> 14;
            s1x14 i = ((int)this->nco_i * (int)this->clk_r + (int)this->nco_r * (int)this->clk_i) >> 14;
            this->nco_r = r;
            this->nco_i = i;
        }
        else
        {
            if (this->clk_i >= s1x14_0_0)
            {
                this->nco_r += this->clk_r;
                if (this->nco_r >= s1x14_1_0)
                {
                    if (wave == triangle)
                    {
                        this->clk_i = -s1x14_1_0;
                        this->nco_r = s1x14_1_0 - (this->nco_r - s1x14_1_0);
                    }
                    else
                    {
                        this->nco_r = this->nco_r - s1x14_1_0 - s1x14_1_0;
                    }
                }
            }
            else
            {
                this->nco_r -= this->clk_r;
                if (this->nco_r <= -s1x14_1_0)
                {
                    if (wave == triangle)
                    {
                        this->clk_i = s1x14_1_0;
                        this->nco_r = -s1x14_1_0 - (this->nco_r + s1x14_1_0);
                    }
                    else
                    {
                        this->nco_r = this->nco_r + s1x14_1_0 + s1x14_1_0;
                    }
                }
            }
        }
    }

    // int r = chan[0].nco_r; // real audio
    int r = 0; // silence
    if (chan[0].wave == square)
    {
        if (r < s1x14_0_0)
        {
            pwm_set_chan_level(AUD_L_SLICE, AUD_L_CHAN, 0);
            pwm_set_chan_level(AUD_R_SLICE, AUD_R_CHAN, 0);
        }
        else
        {
            pwm_set_chan_level(AUD_L_SLICE, AUD_L_CHAN, AUD_PWM_WRAP + 1);
            pwm_set_chan_level(AUD_R_SLICE, AUD_R_CHAN, AUD_PWM_WRAP + 1);
        }
    }
    else
    {
        if (r < s1x14_0_0)
        {
            pwm_set_chan_level(AUD_L_SLICE, AUD_L_CHAN, AUD_PWM_CENTER - (-r >> AUD_SHIFT));
            pwm_set_chan_level(AUD_R_SLICE, AUD_R_CHAN, AUD_PWM_CENTER - (-r >> AUD_SHIFT));
        }
        else
        {
            pwm_set_chan_level(AUD_L_SLICE, AUD_L_CHAN, AUD_PWM_CENTER + (r >> AUD_SHIFT));
            pwm_set_chan_level(AUD_R_SLICE, AUD_R_CHAN, AUD_PWM_CENTER + (r >> AUD_SHIFT));
        }
    }

    static unsigned norm = 1;
    if (!--norm)
    {
        norm = 1 << (AUD_SHIFT - 1);
        for (unsigned idx = 0; idx < AUD_CHANNELS; idx++)
        {
            struct channel *this = &chan[idx];
            enum waveform wave = this->wave;
            if (wave <= sine)
            {
                int r = this->nco_r;
                int i = this->nco_i;
                s1x14 gain = s1x14_1_9r - ((r * r + i * i) >> 14);
                this->nco_r = muls1x14(r, gain);
                this->nco_i = muls1x14(i, gain);
            }
        }
    }
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

    float freq = 440.0; // A4
    float inc = M_PI * 2 * freq / AUD_RATE;
    s1x14 clk_r = float_to_s1x14(cosf(inc));
    s1x14 clk_i = float_to_s1x14(sinf(inc));

    for (unsigned idx = 0; idx < AUD_CHANNELS; idx++)
    {
        pending[idx].nco_r = s1x14_1_0;
        pending[idx].nco_i = s1x14_0_0;
        pending[idx].clk_r = clk_r;
        pending[idx].clk_i = clk_i;
        pending[idx].wave = sine;
        pending[idx].dirty = true;
    }

    pwm_set_irq_enabled(AUD_IRQ_SLICE, true);
    irq_set_exclusive_handler(PWM_IRQ_WRAP, audio_pwm_irq_handler);
    irq_set_enabled(PWM_IRQ_WRAP, true);
}

void aud_reclock(uint32_t sys_clk_khz)
{
    pwm_set_wrap(AUD_IRQ_SLICE, sys_clk_khz / (AUD_RATE / 1000.f));
}

void aud_task(void)
{
    // TODO remove this example
#define TIMEOUT_MS 1500
    static absolute_time_t com_timer;
    static unsigned mode;
    if (absolute_time_diff_us(get_absolute_time(), com_timer) < 0)
    {
        com_timer = delayed_by_us(get_absolute_time(),
                                  TIMEOUT_MS * 1500);
        float freq;
        freq = 440.0; // A4
        // freq = 32.7;     // C1
        // freq = 65.41;    // C2
        // freq = 2093.005; // C7
        // freq = 4186.009; // C8
        float inc = M_PI * 2 * freq / AUD_RATE;
        s1x14 clk_r = float_to_s1x14(cosf(inc));
        s1x14 clk_i = float_to_s1x14(sinf(inc));

        switch (mode)
        {
        case 0:
            mode = 0; // stay
            pending[0].wave = sine;
            pending[0].nco_r = s1x14_1_0;
            pending[0].nco_i = s1x14_0_0;
            pending[0].clk_r = clk_r;
            pending[0].clk_i = clk_i;
            pending[0].dirty = true;
            break;
        case 1:
            mode = 2;
            pending[0].wave = square;
            pending[0].nco_r = s1x14_1_0;
            pending[0].nco_i = s1x14_0_0;
            pending[0].clk_r = clk_r;
            pending[0].clk_i = clk_i;
            pending[0].dirty = true;
            break;
        case 2:
            mode = 4; // skip 3
            pending[0].wave = saw;
            pending[0].nco_r = s1x14_1_0;
            pending[0].clk_r = float_to_s1x14(2 * freq / AUD_RATE);
            pending[0].clk_i = -s1x14_1_0;
            pending[0].dirty = true;
            break;
        case 3:
            mode = 4;
            pending[0].wave = saw;
            pending[0].nco_r = s1x14_1_0;
            pending[0].clk_r = float_to_s1x14(2 * freq / AUD_RATE);
            pending[0].clk_i = +s1x14_1_0;
            pending[0].dirty = true;
            break;
        default:
            mode = 0;
            pending[0].wave = triangle;
            pending[0].nco_r = s1x14_1_0;
            pending[0].clk_r = float_to_s1x14(4 * freq / AUD_RATE);
            pending[0].clk_i = s1x14_1_0;
            pending[0].dirty = true;
            break;
        }
    }
}
