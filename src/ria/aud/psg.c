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
#include <math.h>

#define PSG_RATE 24000
#define PSG_CHANNELS 8

enum adsr_state
{
    attack,
    decay,
    sustain,
    release
};

static volatile uint16_t psg_xaddr;

static const uint32_t vol_table[] = {
    256 << 16,
    224 << 16,
    192 << 16,
    160 << 16,
    128 << 16,
    112 << 16,
    96 << 16,
    80 << 16,
    52 << 16,
    64 << 16,
    40 << 16,
    32 << 16,
    24 << 16,
    16 << 16,
    8 << 16,
    0 << 16,
};

// Same rates as the 6581 SID
static const uint32_t attack_table[] = {
    (1 << 24) / (PSG_RATE / 1000 * 2),
    (1 << 24) / (PSG_RATE / 1000 * 8),
    (1 << 24) / (PSG_RATE / 1000 * 16),
    (1 << 24) / (PSG_RATE / 1000 * 24),
    (1 << 24) / (PSG_RATE / 1000 * 38),
    (1 << 24) / (PSG_RATE / 1000 * 56),
    (1 << 24) / (PSG_RATE / 1000 * 68),
    (1 << 24) / (PSG_RATE / 1000 * 80),
    (1 << 24) / (PSG_RATE / 1000 * 100),
    (1 << 24) / (PSG_RATE / 1000 * 250),
    (1 << 24) / (PSG_RATE / 1000 * 500),
    (1 << 24) / (PSG_RATE / 1000 * 800),
    (1 << 24) / (PSG_RATE / 1000 * 1000),
    (1 << 24) / (PSG_RATE / 1000 * 3000),
    (1 << 24) / (PSG_RATE / 1000 * 5000),
    (1 << 24) / (PSG_RATE / 1000 * 8000),
};

// Same rates as the 6581 SID
static const uint32_t decay_release_table[] = {
    (1 << 24) / (PSG_RATE / 1000 * 6),
    (1 << 24) / (PSG_RATE / 1000 * 24),
    (1 << 24) / (PSG_RATE / 1000 * 48),
    (1 << 24) / (PSG_RATE / 1000 * 72),
    (1 << 24) / (PSG_RATE / 1000 * 114),
    (1 << 24) / (PSG_RATE / 1000 * 168),
    (1 << 24) / (PSG_RATE / 1000 * 204),
    (1 << 24) / (PSG_RATE / 1000 * 240),
    (1 << 24) / (PSG_RATE / 1000 * 300),
    (1 << 24) / (PSG_RATE / 1000 * 750),
    (1 << 24) / (PSG_RATE / 1000 * 1500),
    (1 << 24) / (PSG_RATE / 1000 * 2400),
    (1 << 24) / (PSG_RATE / 1000 * 3000),
    (1 << 24) / (PSG_RATE / 1000 * 9000),
    (1 << 24) / (PSG_RATE / 1000 * 15000),
    (1 << 24) / (PSG_RATE / 1000 * 24000),
};

struct channels
{
    uint16_t duty;
    uint16_t freq;
    uint8_t pan_gate;
    uint8_t vol_attack;
    uint8_t vol_decay;
    uint8_t wave_release;
};

static struct
{
    uint8_t adsr;
    int8_t sample;
    uint32_t vol;
    uint32_t phase;
    uint32_t phase_inc;
    uint32_t noise1;
    uint32_t noise2;
} channel_state[PSG_CHANNELS];

static int8_t sine_table[256];

static void
    __attribute__((optimize("O1")))
    __isr
    __time_critical_func(psg_irq_handler)()
{
    pwm_clear_irq(AUD_IRQ_SLICE);
    struct channels *channels = (void *)&xram[psg_xaddr];
    if (channels == (void *)&xram[0xFFFF])
    {
        pwm_set_chan_level(AUD_L_SLICE, AUD_L_CHAN, AUD_PWM_CENTER);
        pwm_set_chan_level(AUD_R_SLICE, AUD_R_CHAN, AUD_PWM_CENTER);
        return;
    }

    // Output previous sample at start to minimize jitter
    int8_t sample = channel_state[0].sample;
    sample = ((int32_t)sample * (channel_state[0].vol >> 16)) >> 8;
    int8_t pan = (int8_t)channels[0].pan_gate & 0xFE;
    int8_t sample_l = ((int32_t)sample * (128 - pan)) >> 8;
    int8_t sample_r = ((int32_t)sample * (128 + pan)) >> 8;
    pwm_set_chan_level(AUD_L_SLICE, AUD_L_CHAN, sample_l + AUD_PWM_CENTER);
    pwm_set_chan_level(AUD_R_SLICE, AUD_R_CHAN, sample_r + AUD_PWM_CENTER);

    for (unsigned i = 0; i < PSG_CHANNELS; i++)
    {
        // Sample the raw waveform
        channel_state[i].phase += channel_state[i].phase_inc;
        uint32_t phase = channel_state[i].phase >> 16;
        uint32_t duty = channels[i].duty;
        switch (channels[i].wave_release >> 4)
        {
        case 0: // sine
            duty >>= 1;
            if (phase < 32768u - duty || phase >= 32768u + duty)
                channel_state[i].sample = -127;
            else
                channel_state[i].sample = sine_table[phase >> 8];
            break;
        case 1: // square
            if (phase > duty)
                channel_state[i].sample = -127;
            else
                channel_state[i].sample = 127;
            break;
        case 2: // sawtooth
            if (phase > duty)
                channel_state[i].sample = -127;
            else
                channel_state[i].sample = 127 - (phase >> 8);
            break;
        case 3: // triangle
            duty >>= 1;
            if (phase < 32768u - duty || phase >= 32768u + duty)
                channel_state[i].sample = -127;
            else if (phase >= 32768)
                channel_state[i].sample = 127 - (phase >> 7);
            else
                channel_state[i].sample = (phase >> 7) - 128;
            break;
        case 4: // noise
            if ((phase) > duty)
                channel_state[i].sample = -127;
            else
            {
                channel_state[i].noise1 ^= channel_state[i].noise2;
                channel_state[i].sample = channel_state[i].noise2 & 0xFF;
                channel_state[i].noise2 += channel_state[i].noise1;
            }
            break;
        default:
            channel_state[i].sample = 0;
            break;
        }

        // Compute the ADSR envelope volume
        if (!(channels[0].pan_gate & 0x01) && channel_state[i].adsr != release)
            channel_state[i].adsr = release;
        if ((channels[0].pan_gate & 0x01) && channel_state[i].adsr == release)
            channel_state[i].adsr = attack;
        switch (channel_state[i].adsr)
        {
        case attack:
            channel_state[i].vol += attack_table[channels[0].vol_attack & 0xF];
            if (channel_state[i].vol >= vol_table[channels[0].vol_attack >> 4])
            {
                channel_state[i].vol = vol_table[channels[0].vol_attack >> 4];
                channel_state[i].adsr = decay;
            }
            break;
        case decay:
            if (channel_state[i].vol <= decay_release_table[channels[0].vol_decay & 0xF])
                channel_state[i].vol = 0;
            else
                channel_state[i].vol -= decay_release_table[channels[0].vol_decay & 0xF];
            if (channel_state[i].vol <= vol_table[channels[0].vol_decay >> 4])
            {
                channel_state[i].vol = vol_table[channels[0].vol_decay >> 4];
                channel_state[i].adsr = sustain;
            }
            break;
        case sustain:
            break;
        case release:
            if (channel_state[i].vol <= decay_release_table[channels[0].wave_release & 0xF])
                channel_state[i].vol = 0;
            else
                channel_state[i].vol -= decay_release_table[channels[0].wave_release & 0xF];
            break;
        }
    }
}

static void psg_start(void)
{
    // Set up linear-feedback shift register for noise. Starting constants from here:
    // https://www.musicdsp.org/en/latest/Synthesis/216-fast-whitenoise-generator.html
    for (unsigned i = 0; i < PSG_CHANNELS; i++)
    {
        channel_state[i].noise1 = 0x67452301;
        channel_state[i].noise2 = 0xEFCDAB89;
    }

    // Set up sine table
    for (unsigned i = 0; i < 256; i++)
        sine_table[i] = cos(M_PI * 2.0 / 256 * i) * -127;

    irq_set_exclusive_handler(PWM_IRQ_WRAP, psg_irq_handler);
}

static void psg_reclock(uint32_t sys_clk_khz)
{
    pwm_set_wrap(AUD_IRQ_SLICE, sys_clk_khz / (PSG_RATE / 1000.f));
}

static void psg_task(void)
{

    struct channels *channels = (void *)&xram[psg_xaddr];
    if (channels != (void *)&xram[0xFFFF])
    {
        // Perform slow computations on only 1 channel per task
        static unsigned slow_chan = 0;
        if (++slow_chan >= PSG_CHANNELS)
            slow_chan = 0;
        channel_state[slow_chan].phase_inc = ((double)UINT32_MAX + 1) * channels[slow_chan].freq / PSG_RATE;
    }
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
