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

enum psg_adsr_state
{
    attack,
    decay,
    sustain,
    release
};

static volatile uint16_t psg_xaddr;

static const uint32_t psg_vol_table[] = {
    256 << 16,
    204 << 16,
    168 << 16,
    142 << 16,
    120 << 16,
    102 << 16,
    86 << 16,
    73 << 16,
    61 << 16,
    50 << 16,
    40 << 16,
    31 << 16,
    22 << 16,
    14 << 16,
    7 << 16,
    0 << 16,
};

// Same rates as the 6581 SID
static const uint32_t psg_attack_table[] = {
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
static const uint32_t psg_decay_release_table[] = {
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

struct psg_channel
{
    uint16_t freq;
    uint8_t duty;
    uint8_t vol_attack;
    uint8_t vol_decay;
    uint8_t wave_release;
    uint8_t pan_gate;
    uint8_t unused;
};

static struct
{
    int8_t sample;
    uint8_t adsr;
    uint32_t vol;
    uint32_t phase;
    uint32_t phase_inc;
    uint32_t noise1;
    uint32_t noise2;
} psg_channel_state[PSG_CHANNELS];

static int8_t psg_sine_table[256];

static void
    __attribute__((optimize("O1")))
    __isr
    __time_critical_func(psg_irq_handler)()
{
    pwm_clear_irq(AUD_IRQ_SLICE);

    // Check for valid xram address
    struct psg_channel *channels = (void *)&xram[psg_xaddr];
    if (channels == (void *)&xram[0xFFFF])
    {
        pwm_set_chan_level(AUD_L_SLICE, AUD_L_CHAN, AUD_PWM_CENTER);
        pwm_set_chan_level(AUD_R_SLICE, AUD_R_CHAN, AUD_PWM_CENTER);
        return;
    }

    // Output previous sample at start to minimize jitter
    int16_t sample_l = 0;
    int16_t sample_r = 0;
    for (unsigned i = 0; i < PSG_CHANNELS; i++)
    {
        int8_t sample = psg_channel_state[i].sample;
        sample = ((int32_t)sample * (psg_channel_state[i].vol >> 16)) >> 8;
        int8_t pan = (int8_t)channels[i].pan_gate / 2;
        if (pan != -64)
        {

            sample_l += ((int32_t)sample * (63 - pan)) >> 7;
            sample_r += ((int32_t)sample * (63 + pan)) >> 7;
        }
    }
    if (sample_l < -128)
        sample_l = -128;
    if (sample_l > 127)
        sample_l = 127;
    if (sample_r < -128)
        sample_r = -128;
    if (sample_r > 127)
        sample_r = 127;
    pwm_set_chan_level(AUD_L_SLICE, AUD_L_CHAN, sample_l + AUD_PWM_CENTER);
    pwm_set_chan_level(AUD_R_SLICE, AUD_R_CHAN, sample_r + AUD_PWM_CENTER);

    for (unsigned i = 0; i < PSG_CHANNELS; i++)
    {
        // Sample the raw waveform
        psg_channel_state[i].phase += psg_channel_state[i].phase_inc;
        uint32_t phase = psg_channel_state[i].phase >> 24;
        uint32_t duty = channels[i].duty;
        switch (channels[i].wave_release >> 4)
        {
        case 0: // sine
            duty >>= 1;
            if (phase < 128u - duty || phase >= 128u + duty)
                psg_channel_state[i].sample = -127;
            else
                psg_channel_state[i].sample = psg_sine_table[phase];
            break;
        case 1: // square
            if (phase > duty)
                psg_channel_state[i].sample = -127;
            else
                psg_channel_state[i].sample = 127;
            break;
        case 2: // sawtooth
            if (phase > duty)
                psg_channel_state[i].sample = -127;
            else
                psg_channel_state[i].sample = 127 - phase;
            break;
        case 3: // triangle
            duty >>= 1;
            if (phase < 128u - duty || phase >= 128u + duty)
                psg_channel_state[i].sample = -127;
            else if (phase >= 128)
                psg_channel_state[i].sample = 127 - (int8_t)(psg_channel_state[i].phase >> 23);
            else
                psg_channel_state[i].sample = (int8_t)(psg_channel_state[i].phase >> 23) - 128;
            break;
        case 4: // noise
            if (phase > duty)
                psg_channel_state[i].sample = -127;
            else
            {
                psg_channel_state[i].noise1 ^= psg_channel_state[i].noise2;
                psg_channel_state[i].sample = psg_channel_state[i].noise2 & 0xFF;
                psg_channel_state[i].noise2 += psg_channel_state[i].noise1;
            }
            break;
        default:
            psg_channel_state[i].sample = 0;
            break;
        }

        // Compute the ADSR envelope volume
        if (!(channels[i].pan_gate & 0x01) && psg_channel_state[i].adsr != release)
            psg_channel_state[i].adsr = release;
        if ((channels[i].pan_gate & 0x01) && psg_channel_state[i].adsr == release)
            psg_channel_state[i].adsr = attack;
        switch (psg_channel_state[i].adsr)
        {
        case attack:
            psg_channel_state[i].vol += psg_attack_table[channels[i].vol_attack & 0xF];
            if (psg_channel_state[i].vol >= psg_vol_table[channels[i].vol_attack >> 4])
            {
                psg_channel_state[i].vol = psg_vol_table[channels[i].vol_attack >> 4];
                psg_channel_state[i].adsr = decay;
            }
            break;
        case decay:
            if (psg_channel_state[i].vol <= psg_decay_release_table[channels[i].vol_decay & 0xF])
                psg_channel_state[i].vol = 0;
            else
                psg_channel_state[i].vol -= psg_decay_release_table[channels[i].vol_decay & 0xF];
            if (psg_channel_state[i].vol > psg_vol_table[channels[i].vol_decay >> 4])
                break;
            psg_channel_state[i].adsr = sustain;
            __attribute__((fallthrough));
        case sustain:
            if (psg_vol_table[channels[i].vol_decay >> 4] <= psg_vol_table[channels[i].vol_attack >> 4])
                psg_channel_state[i].vol = psg_vol_table[channels[i].vol_decay >> 4];
            break;
        case release:
            if (psg_channel_state[i].vol <= psg_decay_release_table[channels[i].wave_release & 0xF])
                psg_channel_state[i].vol = 0;
            else
                psg_channel_state[i].vol -= psg_decay_release_table[channels[i].wave_release & 0xF];
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
        psg_channel_state[i].noise1 = 0x67452301;
        psg_channel_state[i].noise2 = 0xEFCDAB89;
    }

    // Set up sine table
    for (unsigned i = 0; i < 256; i++)
        psg_sine_table[i] = cos(M_PI * 2.0 / 256 * i) * -127;

    // Set the IRQ handler
    irq_set_exclusive_handler(PWM_IRQ_WRAP, psg_irq_handler);
}

static void psg_reclock(uint32_t sys_clk_khz)
{
    pwm_set_wrap(AUD_IRQ_SLICE, sys_clk_khz / (PSG_RATE / 1000.f));
}

static void psg_task(void)
{

    struct psg_channel *channels = (void *)&xram[psg_xaddr];
    if (channels != (void *)&xram[0xFFFF])
    {
        // Perform slow computations on only 1 channel per task
        static unsigned task_chan = 0;
        if (++task_chan >= PSG_CHANNELS)
            task_chan = 0;
        psg_channel_state[task_chan].phase_inc = ((double)UINT32_MAX + 1) * channels[task_chan].freq / 3 / PSG_RATE;
    }
}

bool psg_xreg(uint16_t word)
{
    if (word & 0x0001 ||
        word > 0x10000 - PSG_CHANNELS * sizeof(struct psg_channel))
    {
        psg_xaddr = 0xFFFF;
        if (word != 0xFFFF)
            return false;
    }
    else
    {
        psg_xaddr = word;
        aud_setup(psg_start, psg_reclock, psg_task);
    }
    return true;
}
