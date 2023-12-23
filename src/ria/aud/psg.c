/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "pico/stdlib.h"
#include "aud.h"
#include "aud/aud.h"
#include "aud/psg.h"
#include "sys/mem.h"
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

struct channels
{
    uint16_t freq;
    uint16_t duty;
    uint8_t trig_wave;
    uint8_t attack_vol;
    uint8_t decay_vol;
    uint8_t release_pan;
};

struct
{
    uint32_t phase;
    uint32_t inc;
} channel_data[PSG_CHANNELS];

static void __isr __time_critical_func(audio_pwm_irq_handler)()
{
    pwm_clear_irq(AUD_IRQ_SLICE);
    struct channels *channels = (void *)&xram[psg_xaddr];
    if (channels == (void *)&xram[0xFFFF])
    {
        pwm_set_chan_level(AUD_L_SLICE, AUD_L_CHAN, AUD_PWM_CENTER);
        pwm_set_chan_level(AUD_R_SLICE, AUD_R_CHAN, AUD_PWM_CENTER);
        return;
    }

    static unsigned slow_chan = 0;
    if (++slow_chan >= PSG_CHANNELS)
        slow_chan = 0;
    channel_data[slow_chan].inc = ((double)UINT32_MAX + 1) * channels[slow_chan].freq / PSG_RATE;

    for (unsigned i = 0; i < PSG_CHANNELS; i++)
    {
        channel_data[i].phase += channel_data[i].inc;
        uint8_t sample;
        switch (channels[i].trig_wave & 0xF)
        {
        case 1: // square
            sample = 255;
            break;
        case 0: // sine (not impl)
        case 2: // sawtooth (not impl)
        case 3: // triangle (not impl)
        default:
            sample = AUD_PWM_CENTER;
            break;
        }
        if ((channel_data[i].phase >> 16) > channels[i].duty)
            sample = 0;
        if (i == 0)
        {
            pwm_set_chan_level(AUD_L_SLICE, AUD_L_CHAN, sample);
            pwm_set_chan_level(AUD_R_SLICE, AUD_R_CHAN, sample);
        }
    }
}

static void psg_start(void)
{
    irq_set_exclusive_handler(PWM_IRQ_WRAP, audio_pwm_irq_handler);
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
    if (word > 0x10000 - PSG_CHANNELS * sizeof(struct channels))
    {
        psg_xaddr = 0xFFFF;
    }
    else
    {
        psg_xaddr = word;
        aud_setup(psg_start, psg_reclock, psg_task);
    }
    return true;
}
