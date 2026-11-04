/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Only the die's pins are used: master clock, CS, WR, RD, IC, A0 and the
 * data bus in; SY, SH and MO, the serial feed to its YM3014B DAC, out. */

#include "opl_lle.h"

#include "fmopl2.h"

#include <stdio.h>
#include <string.h>

void FMOPL2_Clock(fmopl2_t *chip);

/* Master clocks per chip sample and per cycle; a chip sample is 18 cycles. */
#define LLE_FRAME 72
#define LLE_CYCLE 4

/* IC released this many master clocks after the one in which SH falls, the
 * die starts cycle 0 of a chip sample: sample 0. */
#define LLE_RELEASE 15

/* A one-clock WR pulse in the first master clock of cycle C raises the die's
 * internal write strobe from cycle C + 2. The address pulse goes 5 cycles
 * before the data pulse. */
#define LLE_SYNC 2
#define LLE_ADDR_LEAD 5

/* The word latched at SH fall k after the release is sample k - LLE_LAG. */
#define LLE_LAG 1

static fmopl2_t lle;
static int lle_sy, lle_sh;
static uint32_t lle_sr;
static int16_t *lle_out;
static uint32_t lle_falls, lle_samples;

static void lle_half(int level)
{
    lle.input.mclk = level;
    FMOPL2_Clock(&lle);
    /* YM3014B: 13 bits shifted in on SY rising, LSB first, latched on SH
     * falling: a 10-bit offset-binary mantissa and a 3-bit exponent. */
    if (lle.o_sy && !lle_sy)
        lle_sr = (lle_sr >> 1) | (uint32_t)(lle.o_mo & 1) << 12;
    if (!lle.o_sh && lle_sh)
    {
        uint32_t k = lle_falls++;
        if (lle_out && k >= LLE_LAG && k - LLE_LAG < lle_samples)
        {
            int m = (int)(lle_sr & 0x3FF) - 512;
            int e = (int)(lle_sr >> 10) & 7;
            lle_out[k - LLE_LAG] = (int16_t)(m * (1 << (e ? e - 1 : 0)));
        }
    }
    lle_sy = lle.o_sy;
    lle_sh = lle.o_sh;
}

static void lle_clock(void)
{
    lle_half(0);
    lle_half(1);
}

static void lle_pulse(int a0, uint8_t data)
{
    lle.input.address = a0;
    lle.input.data_i = data;
    lle.input.cs = 0;
    lle.input.wr = 0;
    lle_clock();
    lle.input.cs = 1;
    lle.input.wr = 1;
}

static bool lle_global(uint8_t reg)
{
    return reg < 0x20 || reg == 0xBD;
}

/* The cycle of write1, relative to sample n's cycle 0, that makes the die
 * take the write as the engine applies it before sample n: operator
 * registers at the latch of sample n - 2, channel registers in sample n - 1,
 * globals at cycle 0 of sample n. */
static int lle_phase(uint8_t reg)
{
    if (lle_global(reg))
        return 0;
    if (reg >= 0xA0 && reg < 0xD0)
        return -10;
    return -19;
}

static int64_t lle_write1(const opl_write_t *w)
{
    return (int64_t)w->n * 18 + lle_phase(w->reg);
}

bool lle_takes(const opl_write_t *prev, const opl_write_t *w)
{
    return lle_write1(w) - LLE_ADDR_LEAD >= lle_write1(prev) + (lle_global(prev->reg) ? 4 : 19);
}

const char *lle_run(const opl_script_t *schedule, unsigned ic_frames, int16_t *out)
{
    static char why[96];
    const opl_write_t *w = schedule->w;
    size_t count = schedule->count;

    /* Nothing goes on the bus before the release. */
    for (size_t i = 0; i < count; i++)
        if (!(i ? lle_takes(&w[i - 1], &w[i]) : lle_write1(&w[i]) - LLE_ADDR_LEAD >= LLE_SYNC))
        {
            snprintf(why, sizeof why, "write %zu (%02X=%02X at sample %u) is too soon for the bus",
                     i, w[i].reg, w[i].val, (unsigned)w[i].n);
            return why;
        }

    memset(&lle, 0, sizeof lle);
    lle.input.cs = 1;
    lle.input.wr = 1;
    lle.input.rd = 1;
    lle.input.ic = 0;
    lle_sy = lle_sh = 0;
    lle_sr = 0;
    lle_out = NULL;
    for (unsigned i = 0; i < ic_frames * LLE_FRAME; i++)
        lle_clock();
    lle_falls = 0;
    while (!lle_falls)
        lle_clock();
    for (int i = 0; i < LLE_RELEASE; i++)
        lle_clock();
    lle.input.ic = 1;

    lle_out = out;
    lle_samples = schedule->samples;
    lle_falls = 0;
    size_t i = 0;
    bool data = false;
    for (int64_t t = 0; lle_falls < lle_samples + LLE_LAG; t++)
    {
        if (i < count)
        {
            int64_t at = (lle_write1(&w[i]) - LLE_SYNC - (data ? 0 : LLE_ADDR_LEAD)) * LLE_CYCLE;
            if (t == at)
            {
                lle_pulse(data, data ? w[i].val : w[i].reg);
                if (data)
                    i++;
                data = !data;
                continue;
            }
        }
        lle_clock();
    }
    return NULL;
}
