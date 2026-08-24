/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/aud/aud.h"
#include "core/aud/bel.h"
#include "core/aud/opl.h"
#include "core/mem.h"
#include <assert.h>
#include "host.h"
#include <string.h>
#include <emu8950/emu8950.h>

#if defined(DEBUG_RIA_AUD) || defined(DEBUG_RIA_AUD_OPL)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define OPL_CLOCK_RATE 3579552
#define OPL_SAMPLE_RATE 49716

static OPL *opl_emu8950;
static int16_t opl_sample;

#pragma GCC push_options
#pragma GCC optimize("O3")
static void
    HOST_ISR
    HOST_TIME_CRITICAL(opl_irq_handler)(void)
{
    aud_clear_irq();

    // Output previous sample at start to minimize jitter
    aud_out(opl_sample, opl_sample);
    int16_t next;
    OPL_calc_buffer(opl_emu8950, &next, 1);
    /* Four times hot, and the clamp lets the loud parts square off — the
     * machine has always run its OPL this way. It used to reach the same
     * ratio by shifting emu8950's sixteen bits down to ten, which threw
     * six of them away at the source, before any host with a better
     * converter than the RP2350's PWM could see them. Multiplying instead
     * of shifting keeps every bit and clips in exactly the same place. */
    int32_t s = (int32_t)next * 4 + bel_sample(OPL_SAMPLE_RATE);
    if (s < AUD_SAMPLE_MIN)
        s = AUD_SAMPLE_MIN;
    if (s > AUD_SAMPLE_MAX)
        s = AUD_SAMPLE_MAX;
    opl_sample = (int16_t)s;

    // Update opl regs from xram
    uint8_t max_work = 8;
    while (max_work-- && xram_queue_tail != xram_queue_head)
    {
        uint8_t tail = ++xram_queue_tail;
        OPL_writeReg(opl_emu8950,
                     xram_queue[tail][0],
                     xram_queue[tail][1]);
    }
}
#pragma GCC pop_options

bool opl_xreg(uint16_t word)
{
    if (word & 0x00FF)
    {
        /* Giving up control resets the chip and hands the interrupt
         * back, so a stopped program's last chord does not hold. */
        if (opl_emu8950)
            OPL_reset(opl_emu8950);
        aud_stop();
        return word == 0xFFFF;
    }
    // Would be nice to not malloc but initializeTables() is static
    if (!opl_emu8950)
        opl_emu8950 = OPL_new(OPL_CLOCK_RATE, OPL_SAMPLE_RATE);
    assert(opl_emu8950); // OPL_new only fails under memory pressure (a debug build)
    OPL_reset(opl_emu8950);
    xram_queue_page = word >> 8;
    memset((uint8_t *)&xram[word], 0, 256);
    xram_queue_tail = xram_queue_head;
    aud_setup(opl_irq_handler, OPL_SAMPLE_RATE);
    return true;
}
