/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "aud/aud.h"
#include "aud/opl.h"
#include "mon/mon.h"
#include "str/str.h"
#include "sys/mem.h"
#include <pico/stdlib.h>
#include <hardware/pwm.h>
#include <math.h>
#include <string.h>
#include <emu8950/emu8950.h>
#include <cmsis_compiler.h>

#if defined(DEBUG_RIA_AUD) || defined(DEBUG_RIA_AUD_OPL)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define OPL_CLOCK_RATE 3579552
#define OPL_SAMPLE_RATE 49716

static OPL *opl_emu8950;
static volatile uint16_t opl_xaddr;
static int8_t opl_sample;

static void
    __attribute__((optimize("O3")))
    __isr
    __time_critical_func(opl_irq_handler)(void)
{
    pwm_clear_irq(AUD_IRQ_SLICE);

    // Output previous sample at start to minimize jitter
    pwm_set_chan_level(AUD_L_SLICE, AUD_L_CHAN, opl_sample + AUD_PWM_CENTER);
    pwm_set_chan_level(AUD_R_SLICE, AUD_R_CHAN, opl_sample + AUD_PWM_CENTER);
    int16_t next;
    OPL_calc_buffer(opl_emu8950, &next, 1);
    opl_sample = next >> (16 - AUD_PWM_BITS);

    // Update opl regs from xram
    uint8_t max_work = 8;
    while (max_work-- && xram_queue_tail != xram_queue_head)
    {
        ++xram_queue_tail;
        OPL_writeReg(opl_emu8950,
                     xram_queue[xram_queue_tail][0],
                     xram_queue[xram_queue_tail][1]);
    }
}

bool opl_xreg(uint16_t word)
{
    if (word & 0x00FF)
    {
        opl_xaddr = 0xFFFF;
        return word == 0xFFFF;
    }
    // Would be nice to not malloc but initializeTables() is static
    if (!opl_emu8950)
        opl_emu8950 = OPL_new(OPL_CLOCK_RATE, OPL_SAMPLE_RATE);
    if (!opl_emu8950)
    {
        mon_add_response_str(STR_ERR_INTERNAL_ERROR);
        return false;
    }
    OPL_reset(opl_emu8950);
    opl_xaddr = word;
    xram_queue_page = word >> 8;
    memset(&xram[word], 0, 256);
    xram_queue_tail = xram_queue_head;
    aud_setup(opl_irq_handler, OPL_SAMPLE_RATE);
    return true;
}
