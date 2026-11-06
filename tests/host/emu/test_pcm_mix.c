/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The PCM: device on a software machine, through std's ops as a 6502 would
 * make them, heard through aud_render as a sink on the machine's thread
 * takes it. */

#define _USE_MATH_DEFINES

#include "core/api/api.h"
#include "core/api/attr.h"
#include "core/aud/mix.h"
#include "core/aud/pcm.h"
#include "core/sys/sst.h"
#include "core/sys/xram.h"
#include "core/wdc/bus.h"

#include "emu_boot.h"
#include "stdsys.h"

#include <math.h>
#include <string.h>

#define O_WR 0x02
#define O_RD 0x01
#define WHENCE_SET 0
#define WHENCE_CUR 1
#define WHENCE_END 2

/* A frame's sound at the sink's 48 kHz. */
#define FRAME 800

static uint8_t blob[1 << 20];
static float out[FRAME * 2 * 120];

/* Frames run and their sound taken, appended at at. */
static size_t pull(size_t at, int frames)
{
    for (int i = 0; i < frames; i++)
    {
        emu_frames(1);
        at += (size_t)aud_render(&out[at * 2], FRAME);
    }
    return at;
}

/* A table at $1000 of XRAM that plays whole cycles of a tone when looped. */
#define TABLE 0x1000
static uint16_t table_len;

static void make_tone(uint32_t rate, double hz, int bits, int channels, int cycles)
{
    const int n = (int)lround(rate / hz * cycles);
    int at = 0;
    for (int i = 0; i < n; i++)
        for (int c = 0; c < channels; c++)
        {
            const double v = c == 0 ? sin(2.0 * M_PI * hz * i / rate) : 0.0;
            if (bits == 8)
                xram[TABLE + at++] = (uint8_t)lround(128 + 100 * v);
            else
            {
                const int16_t s = (int16_t)lround(16000 * v);
                xram[TABLE + at++] = (uint8_t)s;
                xram[TABLE + at++] = (uint8_t)((uint16_t)s >> 8);
            }
        }
    table_len = (uint16_t)at;
}

/* As much of the looped table as the buffer takes, from where it left off. */
static uint16_t table_at;
static void top_up(int fd)
{
    for (;;)
    {
        const int n = ssys_write_xram(fd, (uint16_t)(TABLE + table_at),
                                      (uint16_t)(table_len - table_at));
        if (n <= 0)
            return;
        table_at = (uint16_t)((table_at + n) % table_len);
        if (table_at)
            return;
    }
}

static void fresh(void)
{
    aud_set_enabled(true);
    aud_stop();
    table_at = 0;
}

UTEST(pcm_mix, it_opens_writes_seeks_and_closes)
{
    fresh();
    ASSERT_TRUE(ssys_open("PCM:22050,16,1", O_RD) < 0);
    ASSERT_TRUE(ssys_open("PCM:96000,16,1", O_WR) < 0);
    const int fd = ssys_open("PCM:22050,16,1", O_WR);
    ASSERT_TRUE(fd >= 0);
    ASSERT_TRUE(ssys_open("PCM:22050,16,1", O_WR) < 0);
    ASSERT_EQ(ssys_lseek(fd, 0, WHENCE_END), PCM_BUF_SIZE);
    ASSERT_EQ(ssys_lseek(fd, 0, WHENCE_CUR), 0);
    ASSERT_EQ(ssys_write_xram(fd, TABLE, 5000), PCM_BUF_SIZE);
    ASSERT_EQ(ssys_write_xram(fd, TABLE, 10), 0);
    ASSERT_EQ(ssys_lseek(fd, 0, WHENCE_CUR), PCM_BUF_SIZE);
    ASSERT_TRUE(ssys_lseek(fd, 1, WHENCE_CUR) < 0);
    ASSERT_EQ(ssys_lseek(fd, 0, WHENCE_SET), 0);
    ASSERT_EQ(ssys_lseek(fd, 0, WHENCE_CUR), 0);
    ASSERT_EQ(ssys_close(fd), 0);
    const int again = ssys_open("PCM:8000,8,2", O_WR);
    ASSERT_TRUE(again >= 0);
    ASSERT_EQ(ssys_close(again), 0);
}

/* What SEEK_CUR says is the machine's: the frames pcm_sample takes from the
 * open on, by the machine's samples, the same muted or not. */
static void level_follows_machine_time(int *utest_result, bool on)
{
    fresh();
    aud_set_enabled(on);
    const int fd = ssys_open("PCM:22050,16,2", O_WR);
    ASSERT_TRUE(fd >= 0);
    const uint64_t opened = bus_now(AUD_NATIVE_RATE);
    ASSERT_EQ(ssys_write_xram(fd, TABLE, PCM_BUF_SIZE), PCM_BUF_SIZE);
    for (int i = 0; i < 6; i++)
    {
        pull(0, 1);
        uint64_t phase = (uint64_t)1 << 32;
        uint64_t takes = pcm_takes(&phase, pcm_step(22050), bus_now(AUD_NATIVE_RATE) - opened);
        if (takes > PCM_BUF_SIZE / 4)
            takes = PCM_BUF_SIZE / 4;
        ASSERT_EQ(ssys_lseek(fd, 0, WHENCE_CUR), (int32_t)(PCM_BUF_SIZE - takes * 4));
    }
    ASSERT_EQ(ssys_lseek(fd, 0, WHENCE_CUR), 0);
    ASSERT_EQ(ssys_close(fd), 0);
    aud_set_enabled(true);
}

UTEST(pcm_mix, the_level_is_the_machines_heard_or_muted)
{
    level_follows_machine_time(utest_result, true);
    level_follows_machine_time(utest_result, false);
}

/* The tone's frequency from its upward zero crossings, and its peak. */
static void tone_of(const float *x, size_t n, int stride, double *hz, double *peak)
{
    int ups = 0;
    size_t first = 0, last = 0;
    double p = 0;
    for (size_t i = 1; i < n; i++)
    {
        const float a = x[(i - 1) * stride], b = x[i * stride];
        if (fabs(b) > p)
            p = fabs(b);
        if (a < 0 && b >= 0)
        {
            if (!ups)
                first = i;
            last = i;
            ups++;
        }
    }
    *hz = ups > 1 ? (ups - 1) * 48000.0 / (double)(last - first) : 0;
    *peak = p;
}

static void play_tone(int *utest_result, int bits, int channels)
{
    fresh();
    make_tone(22050, 1050, bits, channels, 97);
    char name[32];
    snprintf(name, sizeof name, "PCM:22050,%d,%d", bits, channels);
    const int fd = ssys_open(name, O_WR);
    ASSERT_TRUE(fd >= 0);
    size_t n = 0;
    for (int i = 0; i < 60; i++)
    {
        top_up(fd);
        n = pull(n, 1);
    }
    ASSERT_EQ(ssys_lseek(fd, 0, WHENCE_SET), 0);
    ASSERT_EQ(ssys_close(fd), 0);
    /* The first frames ring in from silence; the last ones are skipped too. */
    double hz, peak;
    tone_of(&out[FRAME * 4 * 2], n - FRAME * 8, 2, &hz, &peak);
    ASSERT_LT(fabs(hz - 1050), 1.0);
    const double want = bits == 8 ? 100.0 * 256 / 32768 : 16000.0 / 32768;
    ASSERT_LT(fabs(peak - want), want * 0.02);
    if (channels == 2)
    {
        tone_of(&out[FRAME * 4 * 2 + 1], n - FRAME * 8, 2, &hz, &peak);
        ASSERT_LT(peak, 0.001);
    }
}

UTEST(pcm_mix, a_tone_plays_at_its_pitch_and_level)
{
    play_tone(utest_result, 16, 1);
    play_tone(utest_result, 16, 2);
    play_tone(utest_result, 8, 1);
}

UTEST(pcm_mix, an_underrun_is_a_gap)
{
    fresh();
    make_tone(22050, 1050, 16, 1, 20);
    const int fd = ssys_open("PCM:22050,16,1", O_WR);
    ASSERT_TRUE(fd >= 0);
    ASSERT_EQ(ssys_write_xram(fd, TABLE, 1000), 1000);
    size_t n = pull(0, 4);
    ASSERT_EQ(ssys_lseek(fd, 0, WHENCE_CUR), 0);
    for (size_t i = (n - FRAME) * 2; i < n * 2; i++)
        ASSERT_EQ(out[i], 0.0f);
    ASSERT_EQ(ssys_write_xram(fd, TABLE, 1000), 1000);
    n = pull(0, 1);
    double hz, peak;
    tone_of(out, n, 2, &hz, &peak);
    ASSERT_GT(peak, 0.4);
    ASSERT_EQ(ssys_lseek(fd, 0, WHENCE_SET), 0);
    ASSERT_EQ(ssys_close(fd), 0);
}

/* RIA_ATTR_PCM_UNDERRUNS, as ria_attr_get reads it. */
static uint16_t underruns(void)
{
    API_A = 0x0E;
    attr_api_get();
    return API_AX;
}

/* Each time the buffer runs dry is one underrun, however long it then stays
 * empty, so a one-shot sound adds one as it ends. An emptying lseek is not
 * a take and adds none. Reading the count clears it, and so do an open and
 * a program's end. */
UTEST(pcm_mix, each_time_the_buffer_runs_dry_is_an_underrun)
{
    fresh();
    make_tone(22050, 1050, 16, 1, 20);
    int fd = ssys_open("PCM:22050,16,1", O_WR);
    ASSERT_TRUE(fd >= 0);
    ASSERT_EQ(underruns(), 0);
    ASSERT_EQ(ssys_write_xram(fd, TABLE, 1000), 1000);
    pull(0, 4);
    ASSERT_EQ(underruns(), 1);
    ASSERT_EQ(underruns(), 0);
    pull(0, 2);
    ASSERT_EQ(underruns(), 0);
    for (int i = 0; i < 2; i++)
    {
        ASSERT_EQ(ssys_write_xram(fd, TABLE, 1000), 1000);
        pull(0, 4);
    }
    ASSERT_EQ(underruns(), 2);
    ASSERT_EQ(ssys_write_xram(fd, TABLE, 1000), 1000);
    ASSERT_EQ(ssys_lseek(fd, 0, WHENCE_SET), 0);
    pull(0, 2);
    ASSERT_EQ(underruns(), 0);
    ASSERT_EQ(ssys_write_xram(fd, TABLE, 1000), 1000);
    pull(0, 4);
    ASSERT_EQ(ssys_close(fd), 0);
    fd = ssys_open("PCM:22050,16,1", O_WR);
    ASSERT_TRUE(fd >= 0);
    ASSERT_EQ(underruns(), 0);
    ASSERT_EQ(ssys_write_xram(fd, TABLE, 1000), 1000);
    pull(0, 4);
    aud_stop();
    ASSERT_EQ(underruns(), 0);
    ASSERT_EQ(ssys_close(fd), 0);
}

/* The buffer runs dry with no PCM: call after it, then a save and a load
 * bring the underrun back with the state. */
UTEST(pcm_mix, an_underrun_before_a_save_comes_back)
{
    fresh();
    make_tone(22050, 1050, 16, 1, 20);
    const int fd = ssys_open("PCM:22050,16,1", O_WR);
    ASSERT_TRUE(fd >= 0);
    ASSERT_EQ(ssys_write_xram(fd, TABLE, 1000), 1000);
    pull(0, 4);
    ASSERT_EQ(sst_save(blob, sizeof blob, 0), (const char *)NULL);
    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    ASSERT_EQ(underruns(), 1);
    ASSERT_EQ(ssys_close(fd), 0);
}

/* close and fsync wait, as MIDI: does, until the buffer has played. */
UTEST(pcm_mix, close_waits_for_the_sound)
{
    fresh();
    const int fd = ssys_open("PCM:22050,16,1", O_WR);
    ASSERT_TRUE(fd >= 0);
    ASSERT_EQ(ssys_write_xram(fd, TABLE, PCM_BUF_SIZE), PCM_BUF_SIZE);
    API_A = (uint8_t)fd;
    xstack_ptr = XSTACK_SIZE;
    int frames = 0;
    while (std_api_syncfs())
    {
        pull(0, 1);
        frames++;
    }
    /* 2048 frames at 22050 Hz are 93 ms, five or six frames. */
    ASSERT_GE(frames, 5);
    ASSERT_LE(frames, 7);
    ASSERT_EQ(ssys_lseek(fd, 0, WHENCE_CUR), 0);
    ASSERT_EQ(ssys_write_xram(fd, TABLE, PCM_BUF_SIZE), PCM_BUF_SIZE);
    API_A = (uint8_t)fd;
    xstack_ptr = XSTACK_SIZE;
    frames = 0;
    while (std_api_close())
    {
        pull(0, 1);
        frames++;
    }
    ASSERT_GE(frames, 5);
    ASSERT_LE(frames, 7);
}

/* A program's end takes its sound with it, so std_stop's close is at once. */
UTEST(pcm_mix, a_stop_ends_the_sound)
{
    fresh();
    const int fd = ssys_open("PCM:22050,16,1", O_WR);
    ASSERT_TRUE(fd >= 0);
    ASSERT_EQ(ssys_write_xram(fd, TABLE, PCM_BUF_SIZE), PCM_BUF_SIZE);
    aud_stop();
    ASSERT_EQ(ssys_lseek(fd, 0, WHENCE_CUR), 0);
    ASSERT_EQ(ssys_close(fd), 0);
}

/* A state taken mid-stream comes back with the same level, and plays the
 * same samples to the bit as the run that never left. */
UTEST(pcm_mix, a_savestate_resumes_the_stream)
{
    static float went[FRAME * 2 * 8];
    fresh();
    make_tone(22050, 1050, 16, 2, 97);
    const int fd = ssys_open("PCM:22050,16,2", O_WR);
    ASSERT_TRUE(fd >= 0);
    top_up(fd);
    pull(0, 2);
    top_up(fd);
    const uint16_t saved_at = table_at;
    const int32_t level = ssys_lseek(fd, 0, WHENCE_CUR);
    ASSERT_GT(level, 0);
    ASSERT_EQ(sst_save(blob, sizeof blob, 0), (const char *)NULL);

    size_t n = 0;
    for (int i = 0; i < 6; i++)
    {
        n = pull(n, 1);
        top_up(fd);
    }
    memcpy(went, out, n * 2 * sizeof *out);

    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    table_at = saved_at;
    ASSERT_EQ(ssys_lseek(fd, 0, WHENCE_CUR), level);
    size_t m = 0;
    for (int i = 0; i < 6; i++)
    {
        m = pull(m, 1);
        top_up(fd);
    }
    ASSERT_EQ(m, n);
    ASSERT_EQ(memcmp(went, out, n * 2 * sizeof *out), 0);
    ASSERT_EQ(ssys_lseek(fd, 0, WHENCE_SET), 0);
    ASSERT_EQ(ssys_close(fd), 0);
}

/* A load over a machine with sound still buffered closes it at once, though
 * machine time stands still while it does. */
UTEST(pcm_mix, a_load_over_playing_sound_does_not_wait)
{
    fresh();
    pull(0, 1);
    ASSERT_EQ(sst_save(blob, sizeof blob, 0), (const char *)NULL);
    const int fd = ssys_open("PCM:22050,16,1", O_WR);
    ASSERT_TRUE(fd >= 0);
    ASSERT_EQ(ssys_write_xram(fd, TABLE, PCM_BUF_SIZE), PCM_BUF_SIZE);
    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    ASSERT_TRUE(ssys_lseek(fd, 0, WHENCE_CUR) < 0);
    const int again = ssys_open("PCM:22050,16,1", O_WR);
    ASSERT_TRUE(again >= 0);
    ASSERT_EQ(ssys_close(again), 0);
}

UTEST_MAIN_EMU()
