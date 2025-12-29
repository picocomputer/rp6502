/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "aud/aud.h"
#include "aud/emu8950.h"
#include "aud/opl.h"
#include "mon/mon.h"
#include "str/str.h"
#include "sys/mem.h"
#include <pico/stdlib.h>
#include <hardware/pwm.h>
#include <math.h>
#include <string.h>
#include <cmsis_compiler.h>

#if defined(DEBUG_RIA_AUD) || defined(DEBUG_RIA_AUD_OPL)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

static_assert(AUD_PWM_BITS == 8);

#define OPL_CLOCK_RATE 3579552
#define OPL_SAMPLE_RATE 49716

static OPL *opl_emu8950;
static volatile uint16_t opl_xaddr;
static int8_t opl_sample;
static uint8_t opl_reg_page;

static void
    __attribute__((optimize("O3")))
    __isr
    __time_critical_func(opl_irq_handler)(void)
{
    pwm_clear_irq(AUD_IRQ_SLICE);

    // Check for valid xram address
    if (opl_xaddr == 0xFFFF)
    {
        pwm_set_chan_level(AUD_L_SLICE, AUD_L_CHAN, AUD_PWM_CENTER);
        pwm_set_chan_level(AUD_R_SLICE, AUD_R_CHAN, AUD_PWM_CENTER);
        return;
    }

    // Output previous sample at start to minimize jitter
    pwm_set_chan_level(AUD_L_SLICE, AUD_L_CHAN, opl_sample + AUD_PWM_CENTER);
    pwm_set_chan_level(AUD_R_SLICE, AUD_R_CHAN, opl_sample + AUD_PWM_CENTER);
    int16_t next;
    OPL_calc_buffer(opl_emu8950, &next, 1);
    opl_sample = next >> 8;

    // Update opl regs from xram_dirty_bits[8]
    uint32_t dirty_bits = __LDREXW(&xram_dirty_bits[opl_reg_page]);
    uint32_t status = __STREXW(0, &xram_dirty_bits[opl_reg_page]);
    if (status)
        return;
    int base = opl_reg_page * 32;
    for (int i = 0; i < 32; i++)
        if (dirty_bits & (1 << i))
            OPL_writeReg(opl_emu8950, base + i, xram[opl_xaddr + base + i]);
    if (++opl_reg_page >= 8)
        opl_reg_page = 0;
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
    xram_dirty_page = word >> 8;
    memset(&xram[word], 0, 256);
    memset(xram_dirty_bits, 0, 32);
    aud_setup(opl_irq_handler, OPL_SAMPLE_RATE);
    return true;
}
