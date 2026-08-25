/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/aud/aud.h"
#include "core/aud/bel.h"
#include "core/aud/psg.h"
#include "ria/sys/cpu.h"
#include "ria/sys/sys.h"
#include <math.h>
#include <pico/stdlib.h>
#include <hardware/pwm.h>
#include <hardware/clocks.h>

#if defined(DEBUG_RIA_AUD) || defined(DEBUG_RIA_AUD_AUD)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

/* PWM pin/slice/channel mapping (firmware hardware; formerly in aud.h). */
#define AUD_L_PIN 28
#define AUD_R_PIN 27
#define AUD_PWM_IRQ_PIN 14 /* No IO */
#define AUD_IRQ_SLICE (pwm_gpio_to_slice_num(AUD_PWM_IRQ_PIN))
#define AUD_L_CHAN (pwm_gpio_to_channel(AUD_L_PIN))
#define AUD_L_SLICE (pwm_gpio_to_slice_num(AUD_L_PIN))
#define AUD_R_CHAN (pwm_gpio_to_channel(AUD_R_PIN))
#define AUD_R_SLICE (pwm_gpio_to_slice_num(AUD_R_PIN))

static irq_handler_t aud_irq_fn;
static uint32_t aud_irq_rate;

/* What this chip generates at. The PWM's wrap divides 256 MHz, so 48000
 * lands within 0.006% (a wrap of 5332 realises 48,003 Hz) and the carrier is
 * a separate slice at 250 kHz that does not move with it. At 24000 a square
 * wave aliased everything above 12 kHz straight back into the band, with
 * nothing band-limiting it. */
#define AUD_NATIVE_RATE 48000

uint32_t aud_native_rate(void) { return AUD_NATIVE_RATE; }

void __in_flash("aud_init") aud_init(void)
{
    pwm_config config = pwm_get_default_config();
    pwm_config_set_wrap(&config, ((1u << AUD_PWM_BITS) - 1));
    pwm_init(AUD_L_SLICE, &config, true);
    pwm_init(AUD_R_SLICE, &config, true);
    pwm_init(AUD_IRQ_SLICE, &config, true);

    pwm_set_chan_level(AUD_L_SLICE, AUD_L_CHAN, AUD_PWM_CENTER);
    pwm_set_chan_level(AUD_R_SLICE, AUD_R_CHAN, AUD_PWM_CENTER);

    gpio_set_drive_strength(AUD_L_PIN, GPIO_DRIVE_STRENGTH_2MA);
    gpio_set_drive_strength(AUD_R_PIN, GPIO_DRIVE_STRENGTH_2MA);
    gpio_set_slew_rate(AUD_L_PIN, GPIO_SLEW_RATE_SLOW);
    gpio_set_slew_rate(AUD_R_PIN, GPIO_SLEW_RATE_SLOW);
    gpio_disable_pulls(AUD_L_PIN);
    gpio_disable_pulls(AUD_R_PIN);
    gpio_set_function(AUD_L_PIN, GPIO_FUNC_PWM);
    gpio_set_function(AUD_R_PIN, GPIO_FUNC_PWM);

    aud_sine_init();

    irq_set_priority(PWM_IRQ_WRAP_0, PICO_DEFAULT_IRQ_PRIORITY + 0x10);
    psg_setup(aud_native_rate());
    bel_setup();
}

void aud_stop(void)
{
    bel_setup();
}

void aud_setup(void (*irq_fn)(void), uint32_t rate)
{
    /* The rate is part of the identity. Comparing only the handler made
     * re-registering the same device at a new rate a silent no-op, which is
     * exactly what a host that hands back a different sample rate needs to
     * be able to do. */
    if (aud_irq_fn != irq_fn || aud_irq_rate != rate)
    {
        aud_irq_rate = rate;
        irq_set_enabled(PWM_IRQ_WRAP_0, false);
        pwm_set_irq_enabled(AUD_IRQ_SLICE, false);
        if (aud_irq_fn != NULL)
            irq_remove_handler(PWM_IRQ_WRAP_0, aud_irq_fn);
        aud_irq_fn = irq_fn;
        pwm_clear_irq(AUD_IRQ_SLICE);
        irq_set_exclusive_handler(PWM_IRQ_WRAP_0, irq_fn);
        pwm_set_wrap(AUD_IRQ_SLICE, (SYS_RP2350_KHZ * 1000u) / rate - 1);
        pwm_set_irq_enabled(AUD_IRQ_SLICE, true);
        irq_set_enabled(PWM_IRQ_WRAP_0, true);
    }
}

/* The narrowing, and the only one on the path. A driver hands over a signed
 * sample at full scale; this chip's PWM wraps at 1023, so sixteen bits
 * become ten. Rounded, not floored — a floor here is a systematic half-LSB
 * downward bias on every sample, which is DC, not noise. */
void __time_critical_func(aud_out)(int16_t left, int16_t right)
{
    const int shift = 16 - AUD_PWM_BITS;
    int l = (left + (1 << (shift - 1))) >> shift;
    int r = (right + (1 << (shift - 1))) >> shift;
    if (l > (int)AUD_PWM_CENTER - 1)
        l = AUD_PWM_CENTER - 1;
    if (r > (int)AUD_PWM_CENTER - 1)
        r = AUD_PWM_CENTER - 1;
    pwm_set_chan_level(AUD_L_SLICE, AUD_L_CHAN, l + AUD_PWM_CENTER);
    pwm_set_chan_level(AUD_R_SLICE, AUD_R_CHAN, r + AUD_PWM_CENTER);
}

void __time_critical_func(aud_clear_irq)(void)
{
    pwm_clear_irq(AUD_IRQ_SLICE);
}
