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
#define DBG(...) printf(__VA_ARGS__)
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
    pwm_clear_irq(AUD_IRQ_SLICE);
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
        pwm_set_wrap(AUD_IRQ_SLICE, CPU_RP2350_KHZ / (rate / 1000.f) - 1);
        pwm_set_irq_enabled(AUD_IRQ_SLICE, true);
        irq_set_enabled(PWM_IRQ_WRAP, true);
    }
}

/* TODO

Current audio system allows selection of none, opl, or psg.
We are changing that to bel, opl, or psg.
The new bel device is a single channel and the default.
The opl and psg devices will mix in the bel devices.
So the bel device will need to support variable sample rates
and default to 24000 Hz.

The bel device is not mapped into xram. It will be used from C.
The bel device is similar to the PSG except it has only one mono channel
and will be used from C instead of xram.

The bel api will be:

typedef struct
{
    uint16_t freq;
    uint8_t duty;
    uint8_t vol_attack;
    uint8_t vol_decay;
    uint8_t wave_release;
    uint16_t restrike_ms;
    uint16_t release_ms;
    uint16_t end_ms;
} ria_bel_t;

aud_bel_add(ria_bel_t *sound)

Up to 8 sounds can be queued.

In some cases, we'll play a few notes in sequence
where end_ms is after release_ms and restrike_ms is 0.
The generator will begin in attack and be released in
release_ms with the next note starting end_ms since the start.

In other cases, we'll be "ringing a bell". If restrike_ms is >0
and that amount of time has elapsed and the next sound also has
restrike_ms >0 then the generator will be immediately returned to
attack mode regardless of release_ms or end_ms. If the next sound
is restrike_ms==0, or there is no next sound, then release_ms and
end_ms work normally.

Move the psh shared constants into bel.h.

Make a few "Bell" sounds for me to try out. Small bells like old
typewriters and teletypes have are what I want. Put them in Pico flash.


*/
