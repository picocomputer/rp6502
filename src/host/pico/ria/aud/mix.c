/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/aud/mix.h"
#include "core/aud/opl.h"
#include "core/aud/psg.h"
#include "core/aud/bel.h"
#include "core/aud/pcm.h"
#include "core/sys/ria.h"
#include "ria/sys/ria.h"
#include "ria/sys/rp2350.h"
#include "ria.pio.h"
#include <pico/stdlib.h>
#include <hardware/pio.h>
#include <hardware/pwm.h>

#define AUD_L_PIN 28
#define AUD_R_PIN 27
#define AUD_PWM_IRQ_PIN 14 /* No IO */
#define AUD_IRQ_SLICE (pwm_gpio_to_slice_num(AUD_PWM_IRQ_PIN))
#define AUD_PIO pio2

/* The pins are PIO state machines playing aud_pwm, a carrier period of
 * AUD_PWM_PERIOD clocks for every word in their FIFOs, and every sample is
 * exactly AUD_PWM_PER_SAMPLE of them. A carrier running free of the sample
 * clock slips against it, and every tone grows sidebands. 1030 clocks hold
 * ten bits of level and make a sample 5150 clocks, 49709 Hz at 256 MHz. */
#define AUD_PWM_PERIOD 1030
#define AUD_PWM_PER_SAMPLE 5
#define AUD_PWM_RATE (SYS_RP2350_KHZ * 1000u / (AUD_PWM_PER_SAMPLE * AUD_PWM_PERIOD))
static_assert(AUD_PWM_RATE > AUD_NATIVE_RATE - AUD_NATIVE_RATE / 1000 &&
                  AUD_PWM_RATE < AUD_NATIVE_RATE + AUD_NATIVE_RATE / 1000,
              "the sample rate is within 0.1% of AUD_NATIVE_RATE");

/* A level is the clocks high, 2 to 1022, around a middle of 512. The pulse
 * sits centred in its period: a pulse growing from one edge moves its centre
 * with its width, which is a second harmonic rising with frequency. aud_pwm
 * adds 4 clocks before the first count, 2 to the pulse and 2 after it. */
#define AUD_PWM_MID 512
static inline uint32_t aud_pwm_word(uint32_t level)
{
    const uint32_t lead = AUD_PWM_PERIOD / 2 - 4 - level / 2;
    const uint32_t pulse = level - 2;
    return lead | pulse << 10 | (AUD_PWM_PERIOD - 8 - lead - pulse) << 20;
}

/* Each sample becomes AUD_PWM_PER_SAMPLE levels of ten bits, rounded with
 * the error fed back as (1 - z^-1)^2 at the carrier's rate. That moves the
 * rounding noise above the audio band, where the output's filter takes it,
 * and triangular dither keeps the error from following the signal. The
 * error stays within 95 and the dither within 63, so a sample taken down by
 * 1/64, rounded so -1 stays -1, leaves room for both and no level ever
 * clips. */
static uint32_t aud_dither = 1;
static int32_t aud_err[2][2];
static uint32_t aud_words[2][AUD_PWM_PER_SAMPLE];
static uint aud_sm[2];

__attribute__((optimize("O2"))) static void __time_critical_func(aud_shape)(int32_t sl, int32_t sr)
{
    sl -= (sl + 32) >> 6;
    sr -= (sr + 32) >> 6;
    int32_t l0 = aud_err[0][0], l1 = aud_err[0][1];
    int32_t r0 = aud_err[1][0], r1 = aud_err[1][1];
    uint32_t x = aud_dither;
    for (int i = 0; i < AUD_PWM_PER_SAMPLE; i++)
    {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        const int32_t vl = sl - 2 * l0 + l1;
        const int32_t vr = sr - 2 * r0 + r1;
        const int32_t ql = (vl + (int32_t)(x & 63) + (int32_t)(x >> 6 & 63) - 31) >> 6;
        const int32_t qr = (vr + (int32_t)(x >> 16 & 63) + (int32_t)(x >> 22 & 63) - 31) >> 6;
        l1 = l0;
        l0 = ql * 64 - vl;
        r1 = r0;
        r0 = qr * 64 - vr;
        aud_words[0][i] = aud_pwm_word((uint32_t)(ql + AUD_PWM_MID));
        aud_words[1][i] = aud_pwm_word((uint32_t)(qr + AUD_PWM_MID));
    }
    aud_err[0][0] = l0, aud_err[0][1] = l1;
    aud_err[1][0] = r0, aud_err[1][1] = r1;
    aud_dither = x;
}

/* Every XRAM write core1 queued since the last interrupt, each handed to
 * each engine whose bytes it lands in. It runs before the sample so that a
 * write takes effect on the sample it arrived for, as it does in the fabric.
 * It stops at the count it found on entry: a 6502 writing flat out can queue
 * the next write before an expensive OPL register is applied, and a drain
 * that chased it would never return. The engines cannot move while it runs,
 * because a mapping changes only under aud_engine_lock. The OPL2's writes
 * are gathered and go into its queue in one copy, so a full ring of 40 on
 * the worst sample pays the queue's barriers once. */
__attribute__((optimize("O3"))) static void __time_critical_func(aud_drain)(void)
{
    const uint32_t n = RING_LEN(&ria_aud.ring);
    const uint32_t psg_at = ria_aud.psg_at;
    const uint32_t opl_at = ria_aud.opl_at;
    uint16_t opl_w[RIA_AUD_RING_SIZE];
    uint32_t opl_n = 0;
    for (uint32_t i = 0; i < n; i++)
    {
        const uint32_t pix = *RING_AT(&ria_aud.ring, i);
        const uint32_t addr = pix & 0xFFFF;
        const uint8_t val = (uint8_t)(pix >> 16);
        const uint32_t psg = addr - psg_at;
        const uint32_t opl = addr - opl_at;
        if (psg < PSG_REGS_SIZE)
            psg_put(psg, val);
        if (opl < OPL_REGS_SIZE && opl_is_reg((uint8_t)opl))
            opl_w[opl_n++] = (uint16_t)(opl | val << 8);
    }
    RING_SKIP(&ria_aud.ring, n);
    if (opl_n)
        opl_push_n(opl_w, opl_n);
}

/* The PCM: device's sound. pcm_put on the main loop and the interrupt's
 * pcm_sample share it as pcm.h describes; everything else on it, and
 * aud_pcm_on, changes under aud_engine_lock. */
static pcm_t aud_pcm;
static bool aud_pcm_on;

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

/* The words shaped by the previous interrupt go out first, while each FIFO
 * still holds two, so they play without a gap however long the sample
 * generators take after them. */
static void __isr __time_critical_func(aud_irq)(void)
{
    for (int i = 0; i < AUD_PWM_PER_SAMPLE; i++)
    {
        AUD_PIO->txf[aud_sm[0]] = aud_words[0][i];
        AUD_PIO->txf[aud_sm[1]] = aud_words[1][i];
    }
    pwm_clear_irq(AUD_IRQ_SLICE);

    aud_drain();
    int32_t sl = 0, sr = 0;
    int16_t l, r;
    const uint16_t psg = psg_xaddr_get();
    if (psg != 0xFFFF || !psg_bel_idle())
    {
        psg_sample(&l, &r);
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

    aud_shape(sl, sr);
    aud_load_measure();
}

void __in_flash("aud_init") aud_init(void)
{
    pwm_config config = pwm_get_default_config();
    pwm_config_set_wrap(&config, AUD_PWM_PER_SAMPLE * AUD_PWM_PERIOD - 1);
    pwm_init(AUD_IRQ_SLICE, &config, false);

    const uint offset = pio_add_program(AUD_PIO, &aud_pwm_program);
    const uint pins[2] = {AUD_L_PIN, AUD_R_PIN};
    const uint32_t mid = aud_pwm_word(AUD_PWM_MID);
    for (int i = 0; i < 2; i++)
    {
        aud_sm[i] = (uint)pio_claim_unused_sm(AUD_PIO, true);
        pio_sm_config c = aud_pwm_program_get_default_config(offset);
        sm_config_set_sideset_pins(&c, pins[i]);
        sm_config_set_out_shift(&c, true, false, 32);
        sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
        pio_sm_init(AUD_PIO, aud_sm[i], offset, &c);
        pio_sm_set_consecutive_pindirs(AUD_PIO, aud_sm[i], pins[i], 1, true);
        for (int w = 0; w < 2 + AUD_PWM_PER_SAMPLE; w++)
            pio_sm_put(AUD_PIO, aud_sm[i], mid);
        for (int w = 0; w < AUD_PWM_PER_SAMPLE; w++)
            aud_words[i][w] = mid;
        /* 220 ohms into the filter draws more than 2 mA, and a pad driven
         * past its strength pulls up and down through different
         * resistances, which is a second harmonic. */
        gpio_set_drive_strength(pins[i], GPIO_DRIVE_STRENGTH_12MA);
        gpio_set_slew_rate(pins[i], GPIO_SLEW_RATE_SLOW);
        gpio_disable_pulls(pins[i]);
        pio_gpio_init(AUD_PIO, pins[i]);
    }

#ifdef PICO_SMPS_MODE_PIN
    /* The 3.3 V regulator's power save mode switches in bursts, and their
     * ripple rides out on the audio pins; held high, it switches steadily. */
    gpio_init(PICO_SMPS_MODE_PIN);
    gpio_set_dir(PICO_SMPS_MODE_PIN, GPIO_OUT);
    gpio_put(PICO_SMPS_MODE_PIN, 1);
#endif

    bel_init();

    /* Below the VGA interrupts, above USB and everything else at the
     * default. */
    irq_set_priority(PWM_IRQ_WRAP_0, PICO_DEFAULT_IRQ_PRIORITY - 0x10);
    pwm_clear_irq(AUD_IRQ_SLICE);
    irq_set_exclusive_handler(PWM_IRQ_WRAP_0, aud_irq);
    pwm_set_irq_enabled(AUD_IRQ_SLICE, true);
    irq_set_enabled(PWM_IRQ_WRAP_0, true);
    /* The state machines start half a carrier period after the sample
     * slice, so every interrupt pushes between two of their pulls, when the
     * words placed above are down to two. */
    pwm_set_enabled(AUD_IRQ_SLICE, true);
    while (pwm_get_counter(AUD_IRQ_SLICE) < AUD_PWM_PERIOD / 2)
        tight_loop_contents();
    pio_enable_sm_mask_in_sync(AUD_PIO, (1u << aud_sm[0]) | (1u << aud_sm[1]));
}

void aud_task(void) { bel_task(); }

/* From the main loop, beside aud_irq on core 0. The gate's read and write
 * need no lock, because aud_irq never moves the bell into or out of
 * release. */
void aud_bel_put(unsigned i, uint8_t val) { psg_put(PSG_REGS_SIZE + i, val); }

/* Core1 queues rather than calling aud_xram_write, so the only thing that ever
 * reaches an engine here is aud_irq, and holding the engines still means
 * holding that off. Nothing on this side of the lock runs per write; it is
 * taken to map or park an engine, read a counter, or open, close or empty
 * the PCM: device, and held for well under a sample: two wraps in one hold
 * raise one interrupt, and a sample is lost. Whatever
 * core1 queued before then is applied first, under the mapping it was
 * written for. */
static void aud_engine_lock(void)
{
    irq_set_enabled(PWM_IRQ_WRAP_0, false);
    /* irq_set_enabled posts a write to the NVIC and adds no barrier of its
     * own, so without these the interrupt can still be taken after it
     * returns, which is the one thing this is here to stop. */
    __dsb();
    __isb();
    aud_drain();
}

/* Letting go publishes where the engines now sit, for act_loop to queue
 * by. The interrupt coming back late costs nothing, so it needs no hardware
 * barrier, only the compiler to have stored everything the lock covered,
 * the engines' plain statics among them. */
static void aud_engine_unlock(void)
{
    const uint16_t psg = psg_xaddr_get();
    const uint16_t opl = opl_xaddr_get();
    ria_aud.psg_at = psg == 0xFFFF ? 0x10000 : psg;
    ria_aud.opl_at = opl == 0xFFFF ? 0x10000 : opl;
    __compiler_memory_barrier();
    irq_set_enabled(PWM_IRQ_WRAP_0, true);
}

/* A program's end takes its sound with it, so the close that std_stop makes
 * next has nothing to wait for. */
void aud_stop(void)
{
    aud_engine_lock();
    psg_park();
    opl_park();
    opl_take_drops();
    pcm_clear(&aud_pcm);
    aud_pcm.underruns = 0;
    aud_engine_unlock();
}

/* The lock drains the ring first, so every write core1 queued before the
 * call is counted. */
uint32_t aud_opl_overruns(void)
{
    aud_engine_lock();
    const uint32_t drops = opl_take_drops();
    aud_engine_unlock();
    return drops;
}

uint32_t aud_pcm_underruns(void)
{
    aud_engine_lock();
    const uint32_t underruns = aud_pcm.underruns;
    aud_pcm.underruns = 0;
    aud_engine_unlock();
    return underruns;
}

void aud_psg_map(uint16_t word)
{
    aud_engine_lock();
    psg_map(word);
    aud_engine_unlock();
}

/* opl_reset takes longer than a sample, so it runs between two holds of the
 * lock, on a parked engine that aud_irq leaves alone and core1 queues
 * nothing for. */
void aud_opl_map(uint16_t word)
{
    aud_engine_lock();
    opl_park();
    aud_engine_unlock();
    opl_reset();
    aud_engine_lock();
    opl_xaddr_set(word);
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
