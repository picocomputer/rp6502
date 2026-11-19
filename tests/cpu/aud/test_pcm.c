/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The PCM: device's engine and its rows in std, without a machine. The
 * mixer's side is the stub below, which keeps one pcm_t. */

#define _USE_MATH_DEFINES

#include "core/aud/mix.h"
#include "core/aud/pcm.h"
#include "osal/fs.h"
#include "utest.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pcm_coef.h"

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

/* Row p of PCM_PHASES + 1, the upper half the lower read backwards. */
static int32_t ref_coef(unsigned p, int i)
{
    return p <= PCM_PHASES / 2 ? pcm_coef[p][i]
                               : pcm_coef[PCM_PHASES - p][PCM_TAPS - 1 - i];
}

/* The engine as the plain sum: frames in a flat array, the window the
 * PCM_TAPS newest before the next take, every tap's row looked up and
 * blended where it is used. Against it the engine has to give the same
 * samples to the bit: the history in a doubled ring, the blend done once
 * for both channels, the mirror split three ways. */
static void match_ref(int *utest_result, uint32_t rate, int bits, int channels)
{
    const pcm_fmt_t fmt = {rate, (uint8_t)bits, (uint8_t)channels};
    pcm_t *s = &stub;
    pcm_reset(s, &fmt);
    const uint64_t step = ((uint64_t)rate << 32) / AUD_NATIVE_RATE;
    ASSERT_EQ((uint64_t)s->step, step);

    enum { FRAMES = 6000 };
    const int frame = bits / 8 * channels;
    static uint8_t bytes[FRAMES * 4];
    static int32_t x[2][FRAMES];
    for (int i = 0; i < FRAMES; i++)
    {
        for (int c = 0; c < channels; c++)
        {
            uint8_t *b = &bytes[i * frame + c * (bits / 8)];
            if (bits == 8)
            {
                b[0] = (uint8_t)rnd();
                x[c][i] = ((int32_t)b[0] - 128) * 256;
            }
            else
            {
                const int16_t v = (int16_t)rnd();
                b[0] = (uint8_t)v;
                b[1] = (uint8_t)((uint16_t)v >> 8);
                x[c][i] = v;
            }
        }
        if (channels == 1)
            x[1][i] = x[0][i];
    }

    static int32_t want[2][FRAMES * 8];
    int n_want = 0;
    uint64_t phase = (uint64_t)1 << 32;
    int taken = 0;
    const int sh = PCM_COEF_Q + 15;
    for (;;)
    {
        if (phase >= ((uint64_t)1 << 32))
        {
            if (taken == FRAMES)
                break;
            phase -= (uint64_t)1 << 32;
            taken++;
        }
        const uint32_t mu = (uint32_t)phase;
        const unsigned p = mu >> 25;
        const int32_t f = (int32_t)((mu >> 9) & 0xFFFF);
        for (int c = 0; c < 2; c++)
        {
            int64_t acc = 0;
            for (int i = 0; i < PCM_TAPS; i++)
            {
                const int at = taken - PCM_TAPS + i;
                const int32_t a = ref_coef(p, i), b = ref_coef(p + 1, i);
                const int32_t k = a + (((b - a) * f + 0x8000) >> 16);
                acc += (int64_t)k * (at < 0 ? 0 : x[c][at]);
            }
            want[c][n_want] = (int32_t)((acc * PCM_GAIN + ((int64_t)1 << (sh - 1))) >> sh);
        }
        n_want++;
        phase += step;
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

UTEST(pcm, the_engine_is_the_plain_sum_to_the_bit)
{
    static const uint32_t rates[] = {8000, 11025, 22050, 32000, 44100, 48000};
    for (size_t i = 0; i < sizeof rates / sizeof *rates; i++)
        for (int bits = 8; bits <= 16; bits += 8)
            for (int channels = 1; channels <= 2; channels++)
                match_ref(utest_result, rates[i], bits, channels);
}

/* A tone of amp at hz, 16-bit mono at rate, as n native samples after the
 * filter has filled. */
enum { TONE_N = 16384 };
static int32_t tone[TONE_N];
static void play_tone(uint32_t rate, double hz, double amp)
{
    const pcm_fmt_t fmt = {rate, 16, 1};
    pcm_reset(&stub, &fmt);
    uint32_t i = 0;
    for (int k = -4 * PCM_TAPS * (AUD_NATIVE_RATE / (int)rate + 1); k < TONE_N; k++)
    {
        while (pcm_level(&stub) < PCM_BUF_SIZE - 2)
        {
            const int16_t v = (int16_t)lround(amp * sin(2 * M_PI * hz * i++ / rate));
            const uint8_t b[2] = {(uint8_t)v, (uint8_t)((uint16_t)v >> 8)};
            pcm_put(&stub, b, 2);
        }
        int32_t l, r;
        pcm_sample(&stub, &l, &r);
        if (k >= 0)
            tone[k] = l;
    }
}

/* The amplitude at hz in tone, Hann windowed so a loud tone a few hundred
 * hertz off leaks nothing measurable. */
static double amp_at(double hz)
{
    double re = 0, im = 0, sum = 0;
    for (int k = 0; k < TONE_N; k++)
    {
        const double w = 0.5 - 0.5 * cos(2 * M_PI * k / (TONE_N - 1));
        const double t = 2 * M_PI * hz * k / AUD_NATIVE_RATE;
        re += w * tone[k] * cos(t);
        im += w * tone[k] * sin(t);
        sum += w;
    }
    return 2 * hypot(re, im) / sum;
}

/* A tone at 8 kHz rings with images at every multiple of the rate, either
 * side of it. Each has to be 60 dB under the tone, the one at 4500 Hz just
 * above the input's Nyquist frequency included. */
UTEST(pcm, images_above_the_input_nyquist_frequency_are_stopped)
{
    play_tone(8000, 3500, 16000);
    const double a = amp_at(3500);
    ASSERT_GT(a, 10000.0);
    for (int m = 1; m * 8000 - 3500 < AUD_NATIVE_RATE / 2; m++)
    {
        const int hz[2] = {m * 8000 - 3500, m * 8000 + 3500};
        for (int j = 0; j < 2; j++)
        {
            if (hz[j] >= AUD_NATIVE_RATE / 2)
                continue;
            const double db = 20 * log10(amp_at(hz[j]) / a);
            if (db > -60)
                fprintf(stderr, "  image at %d Hz: %.1f dB\n", hz[j], db);
            ASSERT_LT(db, -60.0);
        }
    }
}

/* Up to 0.7 of the input's Nyquist frequency a tone comes out as loud as it
 * went in, less PCM_GAIN. */
UTEST(pcm, the_passband_is_flat)
{
    static const uint32_t rates[] = {8000, 22050, 44100, 48000};
    for (size_t i = 0; i < sizeof rates / sizeof *rates; i++)
        for (int k = 1; k <= 7; k += 3)
        {
            const double hz = rates[i] / 2.0 * k / 10;
            play_tone(rates[i], hz, 16000);
            const double db = 20 * log10(amp_at(hz) / (16000.0 * PCM_GAIN / 32768));
            if (fabs(db) >= 0.05)
                fprintf(stderr, "  %u Hz, %.0f Hz: %+.3f dB\n", (unsigned)rates[i], hz, db);
            ASSERT_LT(fabs(db), 0.05);
        }
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

/* A level of 0x4000 that runs dry fades to zero without a step bigger than
 * 1/32 of full scale, reaches zero exactly, and plays again. */
UTEST(pcm, an_underrun_fades_to_silence_and_play_resumes)
{
    const pcm_fmt_t fmt = {22050, 16, 1};
    pcm_reset(&stub, &fmt);
    static uint8_t loud[1024];
    for (size_t i = 0; i < sizeof loud; i += 2)
    {
        loud[i] = 0x00;
        loud[i + 1] = 0x40;
    }
    const int32_t steady = 0x4000 * PCM_GAIN / 32768;
    pcm_put(&stub, loud, sizeof loud);
    int32_t l = 0, r = 0;
    while (pcm_level(&stub))
        pcm_sample(&stub, &l, &r);
    ASSERT_LT(abs(l - steady), 4);
    int32_t last = l;
    for (int i = 0; i < 4000; i++)
    {
        pcm_sample(&stub, &l, &r);
        ASSERT_LT(abs(l - last), 1024);
        ASSERT_EQ(l, r);
        last = l;
    }
    ASSERT_EQ(l, 0);
    ASSERT_EQ(r, 0);
    pcm_put(&stub, loud, sizeof loud);
    for (int i = 0; i < 200; i++)
        pcm_sample(&stub, &l, &r);
    ASSERT_LT(abs(l - steady), 4);
    ASSERT_EQ(l, r);
}

/* A clear while it plays fades too. */
UTEST(pcm, a_clear_fades_to_silence)
{
    const pcm_fmt_t fmt = {44100, 16, 2};
    pcm_reset(&stub, &fmt);
    static uint8_t loud[2048];
    for (size_t i = 0; i < sizeof loud; i += 2)
    {
        loud[i] = 0x00;
        loud[i + 1] = 0xC0;
    }
    pcm_put(&stub, loud, sizeof loud);
    int32_t l = 0, r = 0;
    for (int i = 0; i < 300; i++)
        pcm_sample(&stub, &l, &r);
    ASSERT_GT(pcm_level(&stub), 0);
    pcm_clear(&stub);
    int32_t last = l;
    for (int i = 0; i < 4000; i++)
    {
        pcm_sample(&stub, &l, &r);
        ASSERT_LT(abs(l - last), 1024);
        last = l;
    }
    ASSERT_EQ(l, 0);
    ASSERT_EQ(r, 0);
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
