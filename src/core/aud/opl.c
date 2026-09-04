/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/aud/mix.h"
#include "core/aud/opl.h"
#include "core/ria/regs.h"
#include "core/sys/xram.h"
#include <assert.h>
#include <stdatomic.h>
#include <string.h>
#include <emu8950/emu8950.h>

#if defined(DEBUG_AUD) || defined(DEBUG_AUD_OPL)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define OPL_CLOCK_RATE 3579552

static OPL *opl_emu8950;

#pragma GCC push_options
#pragma GCC optimize("O3")
int16_t opl_sample(void)
{
    int16_t next;
    OPL_calc_buffer(opl_emu8950, &next, 1);
    /* Four times hot, and the clamp lets the loud parts square off — the
     * machine has always run its OPL this way. It used to reach the same
     * ratio by shifting emu8950's sixteen bits down to ten, which threw
     * six of them away at the source, before any host with a better
     * converter than the RP2350's PWM could see them. Multiplying instead
     * of shifting keeps every bit and clips in exactly the same place. */
    int32_t s = (int32_t)next * 4;
    if (s < AUD_SAMPLE_MIN)
        s = AUD_SAMPLE_MIN;
    if (s > AUD_SAMPLE_MAX)
        s = AUD_SAMPLE_MAX;

    // Update opl regs from xram
    uint8_t max_work = 8;
    while (max_work-- && xram_queue_tail != xram_queue_head)
    {
        atomic_thread_fence(memory_order_acquire); /* the entry behind the head */
        uint8_t tail = ++xram_queue_tail;
        OPL_writeReg(opl_emu8950,
                     xram_queue[tail][0],
                     xram_queue[tail][1]);
    }
    return (int16_t)s;
}

/* What a mixer registers: the one voice this chip has, on both sides. */
static void opl_stereo(int16_t *left, int16_t *right)
{
    *left = *right = opl_sample();
}
#pragma GCC pop_options

bool opl_xreg(uint16_t word)
{
    if (word & 0x00FF)
    {
        /* Giving up control resets the chip and hands the mix back, so a
         * stopped program's last chord does not hold. */
        if (opl_emu8950)
            OPL_reset(opl_emu8950);
        aud_stop();
        return word == 0xFFFF;
    }
    // Would be nice to not malloc but initializeTables() is static
    if (!opl_emu8950)
        /* A YM3812 samples at its clock over 72, and that is the rate the
         * whole soft machine adopted; AUD_NATIVE_RATE is this chip's number
         * before it is anyone else's. */
        opl_emu8950 = OPL_new(OPL_CLOCK_RATE, AUD_NATIVE_RATE);
    assert(opl_emu8950); // OPL_new only fails under memory pressure (a debug build)
    OPL_reset(opl_emu8950);
    xram_queue_page = word >> 8;
    memset((uint8_t *)&xram[word], 0, 256);
    xram_queue_tail = xram_queue_head;
    aud_setup(opl_stereo);
    return true;
}
