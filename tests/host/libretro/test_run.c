/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/aud/mix.h"
#include "corpus.h"
#include "retro_fe.h"
#include "utest.h"

#include <string.h>

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    fe_open();
    int rc = utest_main(argc, argv);
    fe_close();
    return rc;
}

#define ROM(name) ROMS_DIR "/" name ".rp6502"

static void load(int *utest_result, const char *path)
{
    ASSERT_TRUE(fe_load(path));
}

UTEST(run, one_picture_per_call)
{
    load(utest_result, ROM("mode3_8bpp"));
    fe.video_calls = 0;
    fe_run(120);
    ASSERT_EQ(fe.video_calls, 120);
    fe.unload_game();
}

UTEST(run, the_gamepad_is_polled_before_it_is_read)
{
    load(utest_result, ROM("mode3_8bpp"));
    fe.poll_calls = 0;
    fe.state_calls = 0;
    fe.state_read_before_poll = false;
    fe_run(60);
    ASSERT_EQ(fe.poll_calls, 60);
    ASSERT_TRUE(fe.state_calls > 0);
    ASSERT_FALSE(fe.state_read_before_poll);
    fe.unload_game();
}

UTEST(run, a_second_of_frames_is_a_second_of_sound)
{
    load(utest_result, ROM("mode3_8bpp"));
    fe.audio_calls = 0;
    fe.audio_frames = 0;
    fe_run(60);
    ASSERT_EQ(fe.audio_calls, 60);
    ASSERT_EQ(fe.audio_frames, (size_t)48000);
    fe.unload_game();
}

UTEST(run, a_program_that_plays_something_is_heard)
{
    ASSERT_TRUE(fe_load(AUD_ROM_PSG));
    fe.audio_peak = 0;
    fe_run(20);
    ASSERT_TRUE(fe.audio_peak > 0);
    ASSERT_TRUE(fe.audio_peak <= 32767);
    fe.unload_game();
}

UTEST(run, a_device_at_its_own_rate_still_arrives_at_ours)
{
    ASSERT_TRUE(fe_load(AUD_ROM_OPL));
    fe.audio_peak = 0;
    fe_run(20);
    fe.audio_frames = 0;
    fe_run(60);
    ASSERT_TRUE(fe.audio_peak > 0);

    ASSERT_TRUE(fe.audio_frames > (size_t)47900);
    ASSERT_TRUE(fe.audio_frames < (size_t)48100);
    fe.unload_game();
}

UTEST(run, a_silent_program_still_keeps_time)
{
    ASSERT_TRUE(fe_load(ROM("mode3_8bpp")));
    /* The resampler's history holds 24 samples of whatever the previous
     * program was playing, and only a cold boot clears it, so the first
     * frame's audio still includes about half a millisecond of that sound.
     * This case asserts exact silence, so it starts counting after that
     * frame. */
    fe_run(1);
    fe.audio_peak = 0;
    fe.audio_frames = 0;
    fe_run(60);
    ASSERT_EQ(fe.audio_frames, (size_t)48000);
    ASSERT_EQ(fe.audio_peak, 0);
    fe.unload_game();
}

UTEST(run, nothing_is_handed_over_outside_a_run)
{
    fe.video_calls = fe.audio_calls = fe.poll_calls = 0;
    load(utest_result, ROM("mode3_8bpp"));
    ASSERT_EQ(fe.video_calls, 0);
    ASSERT_EQ(fe.audio_calls, 0);
    ASSERT_EQ(fe.poll_calls, 0);

    fe.reset();
    ASSERT_EQ(fe.video_calls, 0);
    ASSERT_EQ(fe.audio_calls, 0);
    fe.unload_game();
}

UTEST(run, the_frame_is_the_canvas_the_corpus_names)
{
    static const char *names[] = {
        "mode3_8bpp",
        "mode3_1bpp",
        "mode3_4bppr",
        "mode3_16bpp",
    };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
    {
        int w, h;
        ASSERT_TRUE_MSG(corpus_size(names[i], &w, &h), (char *)names[i]);
        char path[512];
        snprintf(path, sizeof path, ROMS_DIR "/%s.rp6502", names[i]);
        ASSERT_TRUE_MSG(fe_load(path), path);
        fe_run(20);
        ASSERT_EQ_MSG((int)fe.frame_w, w, (char *)names[i]);
        ASSERT_EQ_MSG((int)fe.frame_h, h, (char *)names[i]);
        ASSERT_EQ_MSG(fe.frame_pitch, (size_t)w * 4, (char *)names[i]);
        fe.unload_game();
    }
}

UTEST(run, a_smaller_canvas_is_announced)
{
    int w, h;
    ASSERT_TRUE(corpus_size("mode3_1bpp", &w, &h));
    ASSERT_TRUE(fe_load(ROM("mode3_1bpp")));
    fe_run(20);
    ASSERT_TRUE(fe.geom_count > 0);
    ASSERT_EQ((int)fe.geom[fe.geom_count - 1].width, w);
    ASSERT_EQ((int)fe.geom[fe.geom_count - 1].height, h);
    fe.unload_game();
}

UTEST(run, the_frame_never_disagrees_with_the_announcement)
{
    ASSERT_TRUE(fe_load(ROM("mode3_4bppr")));
    fe_run(20);
    ASSERT_TRUE(fe.geom_count > 0);
    ASSERT_EQ(fe.frame_w, fe.geom[fe.geom_count - 1].width);
    ASSERT_EQ(fe.frame_h, fe.geom[fe.geom_count - 1].height);
    fe.unload_game();
}

UTEST(run, a_settled_canvas_is_not_announced_again)
{
    ASSERT_TRUE(fe_load(ROM("mode3_1bpp")));
    fe_run(20);
    int settled = fe.geom_count;
    fe_run(60);
    ASSERT_EQ(fe.geom_count, settled);
    fe.unload_game();
}

UTEST(run, the_pixels_are_where_the_pitch_says)
{
    ASSERT_TRUE(fe_load(ROM("mode3_8bpp")));
    fe_run(20);
    ASSERT_TRUE(fe.frame != NULL);
    ASSERT_EQ(fe.frame_pitch, (size_t)fe.frame_w * 4);
    fe.unload_game();
}

UTEST(run, a_frontend_that_wants_no_video_still_gets_its_call)
{
    ASSERT_TRUE(fe_load(ROM("mode3_8bpp")));
    fe_run(60);
    ASSERT_TRUE(fe.av_enable_asked);

    static uint32_t drawn[640 * 480];
    const size_t px = (size_t)fe.frame_w * fe.frame_h;
    memcpy(drawn, fe.frame_copy, px * sizeof(uint32_t));

    fe.av_enable = RETRO_AV_ENABLE_AUDIO;
    int before = fe.video_calls;
    fe_run(10);
    ASSERT_EQ(fe.video_calls - before, 10);

    fe.av_enable = RETRO_AV_ENABLE_VIDEO | RETRO_AV_ENABLE_AUDIO;
    fe_run(10);
    ASSERT_EQ((size_t)fe.frame_w * fe.frame_h, px);
    fe.unload_game();
}

UTEST(run, the_memory_map_reaches_the_frontend)
{
    fe_close();
    fe_open();
    ASSERT_FALSE(fe.memory_maps_set);
    ASSERT_TRUE(fe_load(ROM("mode3_8bpp")));
    ASSERT_TRUE(fe.memory_maps_set);
    ASSERT_EQ(fe.memory_map_count, 2u);
    fe.unload_game();
}
