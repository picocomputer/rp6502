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
#include "core/sys/xram.h"
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

/* Every XRAM write core1 queued since the last interrupt, each handed to the
 * engine mapped on its page, if one is. It runs before the sample so that a
 * write takes effect on the sample it arrived for, as it does in the fabric.
 * It stops at the head it found on entry: a 6502 writing flat out can queue
 * the next write before an expensive OPL register is applied, and a drain
 * that chased it would never return. The pages cannot move while it runs,
 * because a mapping changes only under aud_engine_lock. At -O3 the OPL2's
 * push is inlined into the loop; at -Os it is a call per write, which a
 * full ring of 40 on the worst sample cannot afford. */
__attribute__((optimize("O3"))) static void __time_critical_func(aud_drain)(void)
{
    uint8_t head = ria_aud.head;
    /* Pairs with the barrier core1 puts before it publishes head. */
    __dmb();
    uint8_t tail = ria_aud.tail;
    uint16_t psg = psg_xaddr_get();
    uint16_t psg_page = psg == 0xFFFF ? 0xFFFF : psg & 0xFF00;
    uint16_t opl_page = opl_xaddr_get();
    while (tail != head)
    {
        tail = (tail + 1) & (RIA_AUD_RING_SIZE - 1);
        const uint32_t pix = ria_aud.entry[tail];
        const uint16_t page = pix & 0xFF00;
        const uint8_t val = (uint8_t)(pix >> 16);
        if (page == psg_page)
            psg_xram_write((uint8_t)pix, val);
        if (page == opl_page)
            opl_push((uint8_t)pix, val);
    }
    ria_aud.tail = tail;
}

/* The blocks the mapped engines are written through: the PSG's channel
 * block, which can straddle two, and the OPL2's page of four. */
static void aud_watch(void)
{
    uint32_t watch[32] = {0};
    const uint16_t psg = psg_xaddr_get();
    if (psg != 0xFFFF)
        for (unsigned b = psg >> 6; b <= (unsigned)(psg + PSG_REGS_SIZE - 1) >> 6; b++)
            watch[b >> 5] |= 1u << (b & 31);
    const uint16_t opl = opl_xaddr_get();
    if (opl != 0xFFFF)
        watch[opl >> 11] |= 0xFu << ((opl >> 6) & 31);
    for (int i = 0; i < 32; i++)
        ria_aud_watch[i] = watch[i];
}

static uint16_t aud_level_l = AUD_PWM_CENTER;
static uint16_t aud_level_r = AUD_PWM_CENTER;

/* How much of each sample period aud_irq uses, for reading over SWD while a
 * program runs: nothing on the console or the 6502 side can see it then.
 * The IRQ slice counts system clocks from the wrap that raised the
 * interrupt, so its counter at the end of aud_irq is the period used,
 * entry latency and higher-priority interrupts included. A wrap flag set
 * again by then is an overrun, and the counter has wrapped with it.
 * Each second is published whole so a reader never sees a half-summed one;
 * peak_max holds until something writes it back to zero. */
static struct
{
    uint32_t samples;
    uint32_t sum;
    uint32_t peak;
    uint32_t overruns;
} aud_load_acc;

volatile struct
{
    uint32_t seconds;
    uint32_t sum;
    uint32_t peak;
    uint32_t overruns;
    uint32_t peak_max;
} aud_load;

static inline void aud_load_measure(void)
{
    uint32_t used = pwm_get_counter(AUD_IRQ_SLICE);
    if (pwm_hw->intr & (1u << AUD_IRQ_SLICE))
    {
        used += pwm_hw->slice[AUD_IRQ_SLICE].top + 1;
        aud_load_acc.overruns++;
    }
    aud_load_acc.sum += used;
    if (used > aud_load_acc.peak)
        aud_load_acc.peak = used;
    if (++aud_load_acc.samples == AUD_NATIVE_RATE)
    {
        aud_load.sum = aud_load_acc.sum;
        aud_load.peak = aud_load_acc.peak;
        aud_load.overruns = aud_load_acc.overruns;
        if (aud_load_acc.peak > aud_load.peak_max)
            aud_load.peak_max = aud_load_acc.peak;
        aud_load.seconds++;
        aud_load_acc.samples = 0;
        aud_load_acc.sum = 0;
        aud_load_acc.peak = 0;
        aud_load_acc.overruns = 0;
    }
}

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
    int32_t sl = bel_sample();
    int32_t sr = sl;
    int16_t l, r;
    const uint16_t psg = psg_xaddr_get();
    if (psg != 0xFFFF)
    {
        psg_sample((const void *)&xram[psg], &l, &r);
        sl += l, sr += r;
    }
    if (opl_xaddr_get() != 0xFFFF)
    {
        const int32_t o = opl_sample();
        sl += o, sr += o;
    }
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
    aud_load_measure();
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
    /* Below the VGA interrupts, above USB and everything else at the
     * default. */
    irq_set_priority(PWM_IRQ_WRAP_0, PICO_DEFAULT_IRQ_PRIORITY - 0x10);
    pwm_clear_irq(AUD_IRQ_SLICE);
    irq_set_exclusive_handler(PWM_IRQ_WRAP_0, aud_irq);
    pwm_set_wrap(AUD_IRQ_SLICE, (SYS_RP2350_KHZ * 1000u) / AUD_NATIVE_RATE - 1);
    pwm_set_irq_enabled(AUD_IRQ_SLICE, true);
    irq_set_enabled(PWM_IRQ_WRAP_0, true);
}

void aud_stop(void)
{
    aud_engine_lock();
    psg_park();
    opl_park();
    aud_watch();
    aud_engine_unlock();
}

/* Core1 queues rather than calling aud_xram_write, so the only thing that ever
 * reaches an engine here is aud_irq, and holding the engines still means
 * holding that off. Nothing on this side of the lock runs per write; it is
 * taken to map, park or reset an engine. Whatever core1 queued before then
 * is applied first, under the mapping it was written for. */
void aud_engine_lock(void)
{
    irq_set_enabled(PWM_IRQ_WRAP_0, false);
    /* irq_set_enabled posts a write to the NVIC and adds no barrier of its
     * own, so without these the interrupt can still be taken after it
     * returns, which is the one thing this is here to stop. Letting go needs
     * no barrier: the interrupt coming back late costs nothing. */
    __dsb();
    __isb();
    aud_drain();
}

void aud_engine_unlock(void) { irq_set_enabled(PWM_IRQ_WRAP_0, true); }

void aud_psg_map(uint16_t word)
{
    aud_engine_lock();
    psg_map(word);
    aud_watch();
    aud_engine_unlock();
}

void aud_opl_map(uint16_t word)
{
    aud_engine_lock();
    opl_map(word);
    aud_watch();
    aud_engine_unlock();
}

