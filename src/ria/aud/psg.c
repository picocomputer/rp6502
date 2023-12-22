/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "pico/stdlib.h"
#include "aud.h"
#include "aud/aud.h"
#include "aud/psg.h"
#include "hardware/pwm.h"

#define PSG_RATE 24000
#define PSG_CHANNELS 8

// Fixed point range of -1.9r to 1.9r for DSP work
typedef signed short s1x14;
#define muls1x14(a, b) ((s1x14)((((int)(a)) * ((int)(b))) >> 14))
#define float_to_s1x14(a) ((s1x14)((a) * 16384.f))
#define s1x14_to_float(a) ((float)(a) / 16384.f)
#define s1x14_0_0 (0)
#define s1x14_1_0 ((s1x14)(1 << 14))
#define s1x14_1_9r ((s1x14)(1 << 15) - 1)

static volatile uint16_t psg_xaddr;

struct {
    uint16_t freq;
    uint16_t duty;
    uint8_t trig_wave;
    uint8_t attack_vol;
    uint8_t decay_vol;
    uint8_t release;
} channels[PSG_CHANNELS];

struct {
    s1x14 phase;
    s1x14 inc;
} channel_data[PSG_CHANNELS];


static void psg_start(void)
{
    // pwm_set_irq_enabled(AUD_IRQ_SLICE, true);
    // irq_set_exclusive_handler(PWM_IRQ_WRAP, audio_pwm_irq_handler);
    // irq_set_enabled(PWM_IRQ_WRAP, true);
}

static void psg_stop(void)
{
    pwm_set_irq_enabled(AUD_IRQ_SLICE, false);
    irq_set_enabled(PWM_IRQ_WRAP, false);
}

static void psg_reclock(uint32_t sys_clk_khz)
{
    pwm_set_wrap(AUD_IRQ_SLICE, sys_clk_khz / (PSG_RATE / 1000.f));
}

static void psg_task(void)
{
}

bool psg_xreg(uint16_t word)
{
    if (word > 0x10000 - sizeof(channels))
    {
        psg_xaddr = 0xFFFF;
    }
    else
    {
        psg_xaddr = word;
        aud_setup(psg_start, psg_stop, psg_reclock, psg_task);
    }
    return true;
}
