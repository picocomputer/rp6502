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
#include "core/aud/pcm.h"
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

/* Every sample lasts exactly AUD_PWM_PER_SAMPLE carrier periods. A carrier
 * running free of the sample clock slips against it, so the levels change
 * up to a carrier period late, by an amount that sweeps, and every tone
 * grows sidebands. A period of 1030 holds the 1024 levels with room to
 * spare and makes a sample 5150 clocks, 49709 Hz at 256 MHz. */
#define AUD_PWM_PERIOD 1030
#define AUD_PWM_PER_SAMPLE 5
#define AUD_PWM_RATE (SYS_RP2350_KHZ * 1000u / (AUD_PWM_PER_SAMPLE * AUD_PWM_PERIOD))
static_assert(AUD_PWM_PERIOD >= (1u << AUD_PWM_BITS), "PWM levels fit the carrier");
static_assert(AUD_PWM_RATE > AUD_NATIVE_RATE - AUD_NATIVE_RATE / 1000 &&
                  AUD_PWM_RATE < AUD_NATIVE_RATE + AUD_NATIVE_RATE / 1000,
              "the sample rate is within 0.1% of AUD_NATIVE_RATE");

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

/* The PCM: device's sound. pcm_put on the main loop and the interrupt's
 * pcm_sample share it as pcm.h describes; everything else on it, and
 * aud_pcm_on, changes under aud_engine_lock. */
static pcm_t aud_pcm;
static bool aud_pcm_on;

static uint16_t aud_level_l = AUD_PWM_CENTER;
static uint16_t aud_level_r = AUD_PWM_CENTER;

/* How much of each sample period aud_irq uses, for reading over SWD while a
 * program runs: nothing on the console or the 6502 side can see it then.
 * The IRQ slice counts system clocks from the wrap that raised the
 * interrupt, so its counter at the end of aud_irq is the period used,
 * entry latency and higher-priority interrupts included. A wrap flag set
 * again by then is an overrun, and the counter has wrapped with it.
 * The pair peak is the most two samples running both used, the less of the
 * two. Each second is published whole so a reader never sees a half-summed
 * one; the maxima hold until something writes them back to zero. */
static struct
{
    uint32_t samples;
    uint32_t sum;
    uint32_t peak;
    uint32_t overruns;
    uint32_t last;
    uint32_t pair;
} aud_load_acc;

volatile struct
{
    uint32_t seconds;
    uint32_t sum;
    uint32_t peak;
    uint32_t overruns;
    uint32_t peak_max;
    uint32_t pair;
    uint32_t pair_max;
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
    const uint32_t pair = used < aud_load_acc.last ? used : aud_load_acc.last;
    aud_load_acc.last = used;
    if (pair > aud_load_acc.pair)
        aud_load_acc.pair = pair;
    if (++aud_load_acc.samples == AUD_NATIVE_RATE)
    {
        aud_load.sum = aud_load_acc.sum;
        aud_load.peak = aud_load_acc.peak;
        aud_load.overruns = aud_load_acc.overruns;
        if (aud_load_acc.peak > aud_load.peak_max)
            aud_load.peak_max = aud_load_acc.peak;
        aud_load.pair = aud_load_acc.pair;
        if (aud_load_acc.pair > aud_load.pair_max)
            aud_load.pair_max = aud_load_acc.pair;
        aud_load.seconds++;
        aud_load_acc.samples = 0;
        aud_load_acc.sum = 0;
        aud_load_acc.peak = 0;
        aud_load_acc.overruns = 0;
        aud_load_acc.pair = 0;
    }
}

/* The levels computed by the previous interrupt are written first, so the
 * write lands at a fixed offset from the interrupt however long the sample
 * generators take after it. The interrupt and the carrier wrap together, so
 * the levels take hold one carrier period later, every sample. The narrowing from sixteen bits to ten rounds
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
    if (aud_pcm_on)
    {
        int32_t pl, pr;
        pcm_sample(&aud_pcm, &pl, &pr);
        sl += pl, sr += pr;
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
    pwm_config_set_wrap(&config, AUD_PWM_PERIOD - 1);
    pwm_init(AUD_L_SLICE, &config, false);
    pwm_init(AUD_R_SLICE, &config, false);
    pwm_config_set_wrap(&config, AUD_PWM_PER_SAMPLE * AUD_PWM_PERIOD - 1);
    pwm_init(AUD_IRQ_SLICE, &config, false);

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

    /* Below the VGA interrupts, above USB and everything else at the
     * default. */
    irq_set_priority(PWM_IRQ_WRAP_0, PICO_DEFAULT_IRQ_PRIORITY - 0x10);
    pwm_clear_irq(AUD_IRQ_SLICE);
    irq_set_exclusive_handler(PWM_IRQ_WRAP_0, aud_irq);
    pwm_set_irq_enabled(AUD_IRQ_SLICE, true);
    irq_set_enabled(PWM_IRQ_WRAP_0, true);
    /* One write starts all three counters from zero on the same clock. */
    hw_set_bits(&pwm_hw->en, (1u << AUD_L_SLICE) | (1u << AUD_R_SLICE) | (1u << AUD_IRQ_SLICE));
}

/* A program's end takes its sound with it, so the close that std_stop makes
 * next has nothing to wait for. */
void aud_stop(void)
{
    aud_engine_lock();
    psg_park();
    opl_park();
    opl_take_drops();
    aud_watch();
    pcm_clear(&aud_pcm);
    aud_engine_unlock();
}

/* The lock drains the ring first, so every write core1 queued before the
 * call is counted. */
uint32_t aud_opl_drops(void)
{
    aud_engine_lock();
    const uint32_t drops = opl_take_drops();
    aud_engine_unlock();
    return drops;
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

bool aud_pcm_open(const pcm_fmt_t *fmt)
{
    if (aud_pcm_on)
        return false;
    aud_engine_lock();
    pcm_reset(&aud_pcm, fmt);
    aud_pcm_on = true;
    aud_engine_unlock();
    return true;
}

void aud_pcm_close(void)
{
    aud_engine_lock();
    aud_pcm_on = false;
    aud_engine_unlock();
}

uint32_t aud_pcm_write(const uint8_t *buf, uint32_t n)
{
    return pcm_put(&aud_pcm, buf, n);
}

uint16_t aud_pcm_level(void)
{
    return pcm_level(&aud_pcm);
}

void aud_pcm_clear(void)
{
    aud_engine_lock();
    pcm_clear(&aud_pcm);
    aud_engine_unlock();
}
