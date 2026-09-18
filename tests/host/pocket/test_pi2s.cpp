/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
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
        dut->clk_mach = 1;
        dut->clk_74a = 1;
        dut->eval();
        dut->clk_mach = 0;
        dut->clk_74a = 0;
        dut->eval();
    }
    dut->arst_n = 1;

    std::vector<int32_t> fed_l, fed_r, dec_l, dec_r;

    int mclk_q = 0, sdiv = 0, sclk_q = 0;
    int lrck_q = 0, bit_in_half = -1;
    uint32_t word = 0;
    long sclk_falls_per_frame = 0;

    /* One sample is fed every 1050 clk_mach cycles, which is 48 kHz at
     * 50.4 MHz. LRCK is the 12.288 MHz MCLK divided by 256, also 48 kHz,
     * so every fed sample must be decoded exactly once. */
    long sample_clk = 0;
    int sample_idx = 0;

    /* The periods 330 and 224 give clk_mach and clk_74a the ratio of
     * 50.4 MHz to 74.25 MHz. */
    long wnext = 330, anext = 224;
    const long T_END = 330L * 1050 * 84;

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
            dut->clk_mach = 1;
            dut->eval();
            dut->clk_mach = 0;
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

            int m = dut->pocket_i2s_mclk;
            if (m && !mclk_q)
                sdiv = (sdiv + 1) & 3;
            mclk_q = m;
            int sclk = sdiv >> 1;
            if (sclk && !sclk_q)
            {
                /* In I2S the MSB follows the LRCK edge by one SCLK, so
                 * the bit sampled with the LRCK change is not a data bit
                 * and pocket_i2s drives it low. */
                int lr = dut->pocket_i2s_lrck;
                if (lr != lrck_q)
                {
                    if (bit_in_half >= 0)
                    {
                        ASSERT_EQ(bit_in_half, 31);
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
        ASSERT_GT(f, (size_t)70);
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
