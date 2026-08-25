/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The bargain retro_run strikes with a frontend.
 *
 * One video frame per call, input polled before it is read, and a second of
 * sound per second of frames. A frontend synchronises on all three, so none
 * of them is a detail — a core that hands over two frames or none has broken
 * the pacing it asked the frontend to do for it.
 *
 * The geometry claims are here too, and their numbers come from the corpus
 * manifest rather than from the machine: a machine agreeing with itself
 * about the wrong canvas is not evidence.
 */

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

UTEST(run, the_pad_is_polled_before_it_is_read)
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

/* A second of frames is a second of sound, whether or not the program made
 * any: a frontend that syncs on audio waits for silence as much as for
 * music. */
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

/* Nothing is handed over outside retro_run. A frontend has not set up a
 * frame yet when it is loading content. */
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

/* The frame handed over is the canvas the corpus says that program uses,
 * and the pitch is that canvas's own — not the largest one's. */
UTEST(run, the_frame_is_the_canvas_the_corpus_names)
{
    static const char *names[] = {
        "mode3_8bpp",   /* 640x480 */
        "mode3_1bpp",   /* 320x240 */
        "mode3_4bppr",  /* 320x180 */
        "mode3_16bpp",  /* 640x360 */
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

/* A canvas that is not the boot console is announced, because a frontend
 * sized its window from av_info and has no other way to hear. */
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

/* And the frame never disagrees with what was announced. */
UTEST(run, the_frame_never_disagrees_with_the_announcement)
{
    ASSERT_TRUE(fe_load(ROM("mode3_4bppr")));
    fe_run(20);
    ASSERT_TRUE(fe.geom_count > 0);
    ASSERT_EQ(fe.frame_w, fe.geom[fe.geom_count - 1].width);
    ASSERT_EQ(fe.frame_h, fe.geom[fe.geom_count - 1].height);
    fe.unload_game();
}

/* The canvas is said once, not once a frame: a frontend is entitled to
 * treat the call as news. */
UTEST(run, a_settled_canvas_is_not_announced_again)
{
    ASSERT_TRUE(fe_load(ROM("mode3_1bpp")));
    fe_run(20);
    int settled = fe.geom_count;
    fe_run(60);
    ASSERT_EQ(fe.geom_count, settled);
    fe.unload_game();
}

/* The core says the machine has a display of its own aspect. */
UTEST(run, the_pixels_are_where_the_pitch_says)
{
    ASSERT_TRUE(fe_load(ROM("mode3_8bpp")));
    fe_run(20);
    ASSERT_TRUE(fe.frame != NULL);
    ASSERT_EQ(fe.frame_pitch, (size_t)fe.frame_w * 4);
    fe.unload_game();
}
