/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The PCM: device's engine and its rows in std, without a machine. The
 * mixer's side is the stub below, which keeps one pcm_t. */

#include "core/aud/mix.h"
#include "core/aud/pcm.h"
#include "core/aud/rsmp.h"
#include "osal/fs.h"
#include "utest.h"

#include <stdio.h>
#include <string.h>

UTEST_MAIN();

/* str.c is not linked, so the name is given here. */
const char STR_PCM_COLON[] = "PCM:";

static pcm_t stub;
static bool stub_open;
static bool stub_cleared;

bool aud_pcm_open(const pcm_fmt_t *fmt)
{
    if (stub_open)
        return false;
    pcm_reset(&stub, fmt);
    stub_open = true;
    return true;
}

void aud_pcm_close(void) { stub_open = false; }
uint32_t aud_pcm_write(const uint8_t *buf, uint32_t n) { return pcm_put(&stub, buf, n); }
uint16_t aud_pcm_level(void) { return pcm_level(&stub); }

void aud_pcm_clear(void)
{
    pcm_clear(&stub);
    stub_cleared = true;
}

static uint32_t rng = 1;
static uint32_t rnd(void)
{
    rng = rng * 1664525u + 1013904223u;
    return rng >> 8;
}

UTEST(pcm, the_format_parses_only_as_given)
{
    pcm_fmt_t f;
    ASSERT_TRUE(pcm_parse("22050,16,2", &f));
    ASSERT_EQ(f.rate, 22050u);
    ASSERT_EQ(f.bits, 16);
    ASSERT_EQ(f.channels, 2);
    ASSERT_TRUE(pcm_parse("1,8,1", &f));
    ASSERT_TRUE(pcm_parse("48000,16,1", &f));
    static const char *const bad[] = {
        "", "x", "0,16,2", "48001,16,2", "22050,24,2", "22050,0,2", "22050,16,3",
        "22050,16,0", "22050,16", "22050,16,2,", "22050, 16,2", " 22050,16,2",
        "22050,16,2 ", "-1,16,2", "22050,,2", "4294967296,16,2",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
        ASSERT_FALSE(pcm_parse(bad[i], &f));
}

/* The engine has to give rsmp's samples to the bit: the half table read
 * backwards for the upper rows, the history in a doubled ring, both
 * channels in one pass. rsmp is set up flat at zero, as pcm_reset leaves
 * the engine, and is fed each channel of the same frames. */
static void match_rsmp(int *utest_result, uint32_t rate, int bits, int channels)
{
    const pcm_fmt_t fmt = {rate, (uint8_t)bits, (uint8_t)channels};
    pcm_t *s = &stub;
    pcm_reset(s, &fmt);
    rsmp_t ref[2];
    for (int c = 0; c < 2; c++)
    {
        rsmp_reset(&ref[c]);
        ref[c].primed = true;
    }
    const uint64_t step = rsmp_step(rate, AUD_NATIVE_RATE);
    ASSERT_EQ((uint64_t)s->step, step);

    enum { FRAMES = 6000 };
    const int frame = bits / 8 * channels;
    static uint8_t bytes[FRAMES * 4];
    static int32_t want[2][FRAMES * 8];
    int n_want = 0;
    for (int i = 0; i < FRAMES; i++)
    {
        int32_t x[2];
        for (int c = 0; c < channels; c++)
        {
            uint8_t *b = &bytes[i * frame + c * (bits / 8)];
            if (bits == 8)
            {
                b[0] = (uint8_t)rnd();
                x[c] = ((int32_t)b[0] - 128) * 256;
            }
            else
            {
                const int16_t v = (int16_t)rnd();
                b[0] = (uint8_t)v;
                b[1] = (uint8_t)((uint16_t)v >> 8);
                x[c] = v;
            }
        }
        if (channels == 1)
            x[1] = x[0];
        int32_t out[2][8];
        int got = 0;
        for (int c = 0; c < 2; c++)
            got = rsmp_push(&ref[c], x[c], step, out[c], 8);
        for (int k = 0; k < got; k++)
        {
            want[0][n_want] = out[0][k];
            want[1][n_want] = out[1][k];
            n_want++;
        }
    }

    /* The bytes go in as room appears, in odd sizes, across the wrap. */
    const uint32_t total = (uint32_t)FRAMES * frame;
    uint32_t fed = 0;
    for (int i = 0; i < n_want; i++)
    {
        while (fed < total && pcm_level(s) < PCM_BUF_SIZE - 64)
        {
            uint32_t n = 1 + rnd() % 97;
            if (n > total - fed)
                n = total - fed;
            fed += pcm_put(s, &bytes[fed], n);
        }
        int32_t l, r;
        pcm_sample(s, &l, &r);
        ASSERT_EQ(l, want[0][i]);
        ASSERT_EQ(r, want[1][i]);
    }
    ASSERT_GT(n_want, FRAMES * (AUD_NATIVE_RATE / (int)rate) - 8);
}

UTEST(pcm, the_engine_is_rsmp_to_the_bit)
{
    static const uint32_t rates[] = {8000, 11025, 22050, 32000, 44100, 48000};
    for (size_t i = 0; i < sizeof rates / sizeof *rates; i++)
        for (int bits = 8; bits <= 16; bits += 8)
            for (int channels = 1; channels <= 2; channels++)
                match_rsmp(utest_result, rates[i], bits, channels);
}

UTEST(pcm, takes_counts_what_the_samples_take)
{
    const pcm_fmt_t fmt = {44100, 16, 1};
    for (int trial = 0; trial < 200; trial++)
    {
        pcm_reset(&stub, &fmt);
        uint64_t phase = stub.phase;
        const uint64_t n = rnd() % 5000;
        const uint64_t takes = pcm_takes(&phase, stub.step, n);
        /* An always-full buffer: what the samples take is what left it. */
        static uint8_t full[PCM_BUF_SIZE];
        uint64_t taken = 0;
        for (uint64_t k = 0; k < n; k++)
        {
            pcm_put(&stub, full, PCM_BUF_SIZE);
            const uint16_t before = pcm_level(&stub);
            int32_t l, r;
            pcm_sample(&stub, &l, &r);
            taken += (uint16_t)(before - pcm_level(&stub)) / 2;
        }
        ASSERT_EQ(takes, taken);
        ASSERT_EQ(phase, stub.phase);
    }
}

UTEST(pcm, an_underrun_is_silence_and_play_resumes)
{
    const pcm_fmt_t fmt = {22050, 16, 1};
    pcm_reset(&stub, &fmt);
    static uint8_t loud[64];
    for (size_t i = 0; i < sizeof loud; i += 2)
    {
        loud[i] = 0x00;
        loud[i + 1] = 0x40;
    }
    pcm_put(&stub, loud, sizeof loud);
    int32_t l, r;
    for (int i = 0; i < 200; i++)
        pcm_sample(&stub, &l, &r);
    ASSERT_EQ(pcm_level(&stub), 0);
    /* Twenty-four silent frames flush the filter. */
    for (int i = 0; i < 200; i++)
        pcm_sample(&stub, &l, &r);
    ASSERT_EQ(l, 0);
    ASSERT_EQ(r, 0);
    pcm_put(&stub, loud, sizeof loud);
    for (int i = 0; i < 40; i++)
        pcm_sample(&stub, &l, &r);
    ASSERT_GT(l, 0x3000);
    ASSERT_EQ(l, r);
}

/* The take that leaves less than a frame is an underrun, once each time
 * the buffer runs dry: a byte short of a frame plays nothing more, and
 * the silent takes after it add none. A clear is not a take, and an open
 * starts the count over. */
UTEST(pcm, a_take_that_runs_the_buffer_dry_is_an_underrun)
{
    const pcm_fmt_t fmt = {22050, 16, 1};
    pcm_reset(&stub, &fmt);
    static const uint8_t frames[7];
    int32_t l, r;
    pcm_put(&stub, frames, 7);
    for (int i = 0; i < 20; i++)
        pcm_sample(&stub, &l, &r);
    ASSERT_EQ(pcm_level(&stub), 1);
    ASSERT_EQ(stub.underruns, 1u);
    for (int i = 0; i < 200; i++)
        pcm_sample(&stub, &l, &r);
    ASSERT_EQ(stub.underruns, 1u);
    pcm_put(&stub, frames, 1);
    for (int i = 0; i < 20; i++)
        pcm_sample(&stub, &l, &r);
    ASSERT_EQ(stub.underruns, 2u);
    pcm_put(&stub, frames, 6);
    pcm_clear(&stub);
    for (int i = 0; i < 20; i++)
        pcm_sample(&stub, &l, &r);
    ASSERT_EQ(stub.underruns, 2u);
    stub.underruns = 0x7FFFFFFF;
    pcm_put(&stub, frames, 2);
    for (int i = 0; i < 20; i++)
        pcm_sample(&stub, &l, &r);
    ASSERT_EQ(stub.underruns, 0x7FFFFFFFu);
    pcm_reset(&stub, &fmt);
    ASSERT_EQ(stub.underruns, 0u);
}

UTEST(pcm, a_put_takes_what_fits)
{
    const pcm_fmt_t fmt = {48000, 16, 2};
    pcm_reset(&stub, &fmt);
    static uint8_t big[PCM_BUF_SIZE + 100];
    ASSERT_EQ(pcm_put(&stub, big, sizeof big), (uint32_t)PCM_BUF_SIZE);
    ASSERT_EQ(pcm_put(&stub, big, 1), 0u);
    ASSERT_EQ(pcm_level(&stub), PCM_BUF_SIZE);
    pcm_clear(&stub);
    ASSERT_EQ(pcm_level(&stub), 0);
}

UTEST(pcm, the_device_opens_writes_seeks_and_closes)
{
    api_errno err = API_EIO;
    stub_open = false;
    ASSERT_TRUE(pcm_std_handles("PCM:22050,16,2"));
    ASSERT_TRUE(pcm_std_handles("pcm:"));
    ASSERT_FALSE(pcm_std_handles("PCMX:"));
    ASSERT_EQ(pcm_std_open("PCM:22050,16,2", FS_RD, &err), -1);
    ASSERT_EQ(err, API_EINVAL);
    ASSERT_EQ(pcm_std_open("PCM:22050,16,2", FS_RD | FS_WR, &err), -1);
    ASSERT_EQ(err, API_EINVAL);
    ASSERT_EQ(pcm_std_open("PCM:22050,24,2", FS_WR, &err), -1);
    ASSERT_EQ(err, API_EINVAL);
    const int desc = pcm_std_open("PCM:22050,16,2", FS_WR, &err);
    ASSERT_TRUE(desc >= 0);
    ASSERT_EQ(pcm_std_open("PCM:22050,16,2", FS_WR, &err), -1);
    ASSERT_EQ(err, API_EBUSY);

    static char data[5000];
    uint32_t wrote = 0;
    ASSERT_EQ(pcm_std_write(desc, data, sizeof data, &wrote, &err), STD_OK);
    ASSERT_EQ(wrote, (uint32_t)PCM_BUF_SIZE);
    int32_t pos = -1;
    ASSERT_EQ(pcm_std_lseek(desc, SEEK_CUR, 0, &pos, &err), 0);
    ASSERT_EQ(pos, PCM_BUF_SIZE);
    ASSERT_EQ(pcm_std_lseek(desc, SEEK_END, 0, &pos, &err), 0);
    ASSERT_EQ(pos, PCM_BUF_SIZE);
    ASSERT_EQ(pcm_std_lseek(desc, SEEK_CUR, 1, &pos, &err), -1);
    ASSERT_EQ(err, API_EINVAL);
    ASSERT_EQ(pcm_std_lseek(desc, SEEK_SET, 4, &pos, &err), -1);
    ASSERT_EQ(pcm_std_lseek(desc, 7, 0, &pos, &err), -1);
    ASSERT_EQ(err, API_EINVAL);

    ASSERT_EQ(pcm_std_sync(desc, &err), STD_PENDING);
    ASSERT_EQ(pcm_std_close(desc, &err), STD_PENDING);
    ASSERT_TRUE(stub_open);

    stub_cleared = false;
    ASSERT_EQ(pcm_std_lseek(desc, SEEK_SET, 0, &pos, &err), 0);
    ASSERT_EQ(pos, 0);
    ASSERT_TRUE(stub_cleared);
    /* Three bytes are less than a stereo 16-bit frame and never play. */
    ASSERT_EQ(pcm_std_write(desc, data, 3, &wrote, &err), STD_OK);
    ASSERT_EQ(pcm_std_sync(desc, &err), STD_OK);
    ASSERT_EQ(pcm_std_close(desc, &err), STD_OK);
    ASSERT_FALSE(stub_open);
}
