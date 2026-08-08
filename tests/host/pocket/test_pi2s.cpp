/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * pocket_i2s decoded like the codec hears it: the bench feeds 48 kHz
 * samples on the machine clock at the true 165:224 ratio against
 * 74.25 MHz, reconstructs SCLK from the MCLK port, samples the data
 * line on rising edges, and rebuilds each channel word. The frames
 * must run 64 SCLK with sixteen active bits MSB-first, right under
 * LRCK high as the shipped cores transmit; the stream must equal the
 * fed sequence exactly — every sample heard exactly ONCE.
 *
 * THE CODEC MODELLED HERE IS OURS. Analogue names no part and
 * publishes no waveform; the frame format above was read off other
 * people's shipped cores and may be wrong in ways this bench cannot
 * see, because it is the same assumption on both sides.
 *
 * Once, because the machine is 48 kHz end to end now: the PSG and the
 * bell generate at it and the OPL is resampled into it upstream, so
 * this stage is 1:1 and any drop or repeat here is a bug. It used to
 * be fed 24 kHz and asserted every sample twice, which was exact for
 * the PSG and silently never tested the OPL path that dropped 1,700
 * samples a second. Both ends are exactly 48,000 on average — the
 * producer is 50.4 MHz/1050 and LRCK is 12.288 MHz/256 off the
 * fractional MCLK — so there is no drift for the FIFO to absorb, only
 * jitter.
 */

#include "Vpocket_i2s.h"

#include "utest.h"

#include <vector>

static Vpocket_i2s *dut;

UTEST(pi2s, frames_and_samples_exact)
{
    dut->arst_n = 0;
    for (int i = 0; i < 4; i++)
    {
        dut->clk_sys = 1;
        dut->clk_74a = 1;
        dut->eval();
        dut->clk_sys = 0;
        dut->clk_74a = 0;
        dut->eval();
    }
    dut->arst_n = 1;

    /* Fed and decoded streams, as signed left/right pairs. */
    std::vector<int32_t> fed_l, fed_r, dec_l, dec_r;

    /* Decoder state. */
    int mclk_q = 0, sdiv = 0, sclk_q = 0;
    int lrck_q = 0, bit_in_half = -1;
    uint32_t word = 0;
    long sclk_falls_per_frame = 0;

    /* Machine-side sample cadence: one sample per 1050 clk_sys — 48 kHz. */
    long sample_clk = 0;
    int sample_idx = 0;

    /* clk_sys period 330, clk_74a period 224 — the true ratio. */
    long wnext = 330, anext = 224;
    const long T_END = 330L * 1050 * 84; /* 80 samples' worth + startup */

    for (long t = 0; t < T_END; t++)
    {
        bool wedge = t == wnext;
        bool aedge = t == anext;
        if (!wedge && !aedge)
            continue;

        if (wedge)
        {
            dut->aud_valid = 0;
            if (++sample_clk == 1050)
            {
                sample_clk = 0;
                /* Distinct consecutive values, extremes included. Signed
                 * at full scale now: the machine hands the codec sixteen
                 * bits and this stage passes them through, so a value that
                 * survives must arrive bit for bit. The old feed was ten
                 * bits scaled up six, which could not have caught a stage
                 * that quietly dropped the bottom six. */
                int l = (int)(int16_t)(sample_idx * 4801 + 13);
                int r = (int)(int16_t)(-3 - sample_idx * 7919);
                if (sample_idx == 5)
                    l = -32768;
                if (sample_idx == 6)
                    l = 32767;
                dut->aud_l = (int16_t)l;
                dut->aud_r = (int16_t)r;
                dut->aud_valid = 1;
                fed_l.push_back((int16_t)l);
                fed_r.push_back((int16_t)r);
                sample_idx++;
            }
            dut->clk_sys = 1;
            dut->eval();
            dut->clk_sys = 0;
            dut->eval();
            wnext += 330;
        }

        if (aedge)
        {
            dut->clk_74a = 1;
            dut->eval();
            dut->clk_74a = 0;
            dut->eval();
            anext += 224;

            /* Reconstruct SCLK the way the codec does, from MCLK. */
            int m = dut->pocket_i2s_mclk;
            if (m && !mclk_q)
                sdiv = (sdiv + 1) & 3;
            mclk_q = m;
            int sclk = sdiv >> 1;
            if (sclk && !sclk_q)
            {
                /* Rising edge: the launched bit is stable. The MSB
                 * rides one SCLK behind the LRCK edge — the I2S delay
                 * — so the edge rise itself carries a dummy zero. */
                int lr = dut->pocket_i2s_lrck;
                if (lr != lrck_q)
                {
                    if (bit_in_half >= 0)
                    {
                        ASSERT_EQ(bit_in_half, 31);
                        /* The finished half: right under LRCK high,
                         * left under low — the reference's slots. */
                        if (lrck_q)
                            dec_r.push_back((int16_t)word);
                        else
                            dec_l.push_back((int16_t)word);
                    }
                    bit_in_half = 0;
                    word = 0;
                    lrck_q = lr;
                    ASSERT_EQ(dut->pocket_i2s_dac, 0);
                }
                else if (bit_in_half >= 0)
                {
                    bit_in_half++;
                    if (bit_in_half <= 16)
                        word = (word << 1) | (uint32_t)dut->pocket_i2s_dac;
                    else
                        ASSERT_EQ(dut->pocket_i2s_dac, 0);
                    sclk_falls_per_frame++;
                }
            }
            sclk_q = sclk;
        }
    }

    /* Every fed sample heard exactly once, in order, after the silent
     * start-up frames. No dedup: a repeat is the FIFO underrunning and a
     * skip is it overflowing, and both used to hide inside "twice". */
    auto check = [&](std::vector<int32_t> &dec, std::vector<int32_t> &fed) {
        size_t d = 0;
        while (d < dec.size() && dec[d] == 0 && (fed.empty() || fed[0] != 0))
            d++;
        size_t f = 0;
        while (d < dec.size() && f < fed.size())
        {
            ASSERT_EQ(dec[d], fed[f]);
            d++;
            f++;
        }
        ASSERT_GT(f, (size_t)70); /* nearly all 80 heard, once each */
    };
    check(dec_l, fed_l);
    check(dec_r, fed_r);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vpocket_i2s;
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
