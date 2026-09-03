/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/aud/mix.h"
#include "core/aud/bel.h"
#include "core/aud/sine.h"
#include "ria/sys/rp2350.h"
#include <pico/stdlib.h>
#include <hardware/pwm.h>
#include <hardware/clocks.h>

#if defined(DEBUG_AUD) || defined(DEBUG_AUD_MIX)
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

/* The device to mix, or none. */
static void (*aud_dev)(int16_t *left, int16_t *right);

/* The pair the last interrupt mixed, already narrowed to the PWM's ten
 * bits, for the next interrupt to write before it computes anything.
 * Centre to begin with, which is where aud_init parks the pins. */
static uint16_t aud_level_l = AUD_PWM_CENTER;
static uint16_t aud_level_r = AUD_PWM_CENTER;

/* One sample per PWM wrap. The write comes first, so it lands at a fixed
 * offset from the interrupt whatever the generators cost after it; what it
 * writes is the pair the previous interrupt made. Then the mix: the device
 * if one is registered, the bell regardless, clamped, and narrowed --
 * sixteen bits to ten, and rounded, not floored, because a floor here is a
 * systematic half-LSB downward bias on every sample, which is DC, not
 * noise. This is the only narrowing on the path. */
static void __isr __time_critical_func(aud_irq)(void)
{
    pwm_set_chan_level(AUD_L_SLICE, AUD_L_CHAN, aud_level_l);
    pwm_set_chan_level(AUD_R_SLICE, AUD_R_CHAN, aud_level_r);
    pwm_clear_irq(AUD_IRQ_SLICE);

    int16_t l = 0, r = 0;
    if (aud_dev)
        aud_dev(&l, &r);
    const int32_t bel = bel_sample();
    int32_t sl = l + bel;
    int32_t sr = r + bel;
    if (sl < AUD_SAMPLE_MIN)
        sl = AUD_SAMPLE_MIN;
    if (sl > AUD_SAMPLE_MAX)
        sl = AUD_SAMPLE_MAX;
    if (sr < AUD_SAMPLE_MIN)
        sr = AUD_SAMPLE_MIN;
    if (sr > AUD_SAMPLE_MAX)
        sr = AUD_SAMPLE_MAX;

    const int shift = 16 - AUD_PWM_BITS;
    int nl = (sl + (1 << (shift - 1))) >> shift;
    int nr = (sr + (1 << (shift - 1))) >> shift;
    if (nl > (int)AUD_PWM_CENTER - 1)
        nl = AUD_PWM_CENTER - 1;
    if (nr > (int)AUD_PWM_CENTER - 1)
        nr = AUD_PWM_CENTER - 1;
    aud_level_l = (uint16_t)(nl + AUD_PWM_CENTER);
    aud_level_r = (uint16_t)(nr + AUD_PWM_CENTER);
}

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

    sine_init();
    bel_init();

    /* One interrupt at AUD_NATIVE_RATE, installed once. The wrap divides
     * the part's clock -- 5149 realises 49718 Hz, 0.005% over -- and the
     * carrier is a separate slice at 250 kHz that does not move with it. */
    irq_set_priority(PWM_IRQ_WRAP_0, PICO_DEFAULT_IRQ_PRIORITY + 0x10);
    pwm_clear_irq(AUD_IRQ_SLICE);
    irq_set_exclusive_handler(PWM_IRQ_WRAP_0, aud_irq);
    pwm_set_wrap(AUD_IRQ_SLICE, (SYS_RP2350_KHZ * 1000u) / AUD_NATIVE_RATE - 1);
    pwm_set_irq_enabled(AUD_IRQ_SLICE, true);
    irq_set_enabled(PWM_IRQ_WRAP_0, true);
}

void aud_stop(void)
{
    aud_dev = NULL;
}

void aud_setup(void (*sample)(int16_t *left, int16_t *right))
{
    aud_dev = sample;
}
