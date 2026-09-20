/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/aud/mix.h"
#include "core/aud/opl.h"
#include "core/aud/psg.h"
#include "core/aud/bel.h"
#include "core/aud/sine.h"
#include "core/sys/ria.h"
#include "ria/sys/ria.h"
#include "ria/sys/rp2350.h"
#include <pico/stdlib.h>
#include <hardware/pwm.h>
#include <hardware/clocks.h>

#define AUD_L_PIN 28
#define AUD_R_PIN 27
#define AUD_PWM_IRQ_PIN 14 /* No IO */
#define AUD_IRQ_SLICE (pwm_gpio_to_slice_num(AUD_PWM_IRQ_PIN))
#define AUD_L_CHAN (pwm_gpio_to_channel(AUD_L_PIN))
#define AUD_L_SLICE (pwm_gpio_to_slice_num(AUD_L_PIN))
#define AUD_R_CHAN (pwm_gpio_to_channel(AUD_R_PIN))
#define AUD_R_SLICE (pwm_gpio_to_slice_num(AUD_R_PIN))

static aud_dev_t aud_dev;

/* Everything the RW engine left since the last interrupt, which is a dozen
 * writes from a 6502 in a tight store loop and nothing at all the rest of the
 * time. It runs before the sample so that a write takes effect on the sample
 * it arrived for, as it does in the fabric. */
static void aud_drain(void)
{
    uint8_t loc, val;
    while (ria_aud_next(&loc, &val))
        switch (aud_dev)
        {
        case aud_dev_psg: psg_xram_write(loc, val); break;
        case aud_dev_opl: opl_xram_write(loc, val); break;
        case aud_dev_none: break;
        }
}

static uint16_t aud_level_l = AUD_PWM_CENTER;
static uint16_t aud_level_r = AUD_PWM_CENTER;

/* The levels computed by the previous interrupt are written first, so the
 * write lands at a fixed offset from the interrupt however long the sample
 * generators take after it. The narrowing from sixteen bits to ten rounds
 * rather than floors, because a floor biases every sample down by half a
 * step on average, which is a DC offset and not noise. */
static void __isr __time_critical_func(aud_irq)(void)
{
    pwm_set_chan_level(AUD_L_SLICE, AUD_L_CHAN, aud_level_l);
    pwm_set_chan_level(AUD_R_SLICE, AUD_R_CHAN, aud_level_r);
    pwm_clear_irq(AUD_IRQ_SLICE);

    aud_drain();
    int16_t l = 0, r = 0;
    switch (aud_dev)
    {
    case aud_dev_psg: psg_sample(&l, &r); break;
    case aud_dev_opl: opl_stereo(&l, &r); break;
    case aud_dev_none: break;
    }
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

    /* The IRQ slice has a period of 5149 system clocks, which is 49718 Hz and
     * 0.005% above AUD_NATIVE_RATE. The L and R pins are on other slices, so
     * their 250 kHz carrier does not change with the IRQ slice's period. */
    irq_set_priority(PWM_IRQ_WRAP_0, PICO_DEFAULT_IRQ_PRIORITY + 0x10);
    pwm_clear_irq(AUD_IRQ_SLICE);
    irq_set_exclusive_handler(PWM_IRQ_WRAP_0, aud_irq);
    pwm_set_wrap(AUD_IRQ_SLICE, (SYS_RP2350_KHZ * 1000u) / AUD_NATIVE_RATE - 1);
    pwm_set_irq_enabled(AUD_IRQ_SLICE, true);
    irq_set_enabled(PWM_IRQ_WRAP_0, true);
}

void aud_stop(void)
{
    aud_dev = aud_dev_none;
    ria_aud_watch(0xFFFF);
}

void aud_setup(aud_dev_t dev)
{
    aud_dev = dev;
}

/* Core1 queues rather than calling aud_xram_write, so the only thing that ever
 * reaches an engine here is aud_irq, and holding the engines still means
 * holding that off. Nothing on this side of the lock runs per write; it is
 * taken to reset a chip, which silences it anyway. */
void aud_engine_lock(void)
{
    irq_set_enabled(PWM_IRQ_WRAP_0, false);
    /* irq_set_enabled posts a write to the NVIC and adds no barrier of its
     * own, so without these the interrupt can still be taken after it
     * returns, which is the one thing this is here to stop. Letting go needs
     * no barrier: the interrupt coming back late costs nothing. */
    __dsb();
    __isb();
}

void aud_engine_unlock(void) { irq_set_enabled(PWM_IRQ_WRAP_0, true); }

aud_dev_t aud_device(void) { return aud_dev; }
