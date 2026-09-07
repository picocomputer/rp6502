/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The savestate, as a frontend uses it.
 *
 * Not the blob's own framing -- that is tests/host/emu/test_sst.c, which can
 * see inside it. What this suite is about is the bargain: a size that can be
 * allocated against, a machine that comes back running, a refusal that leaves
 * the core standing, and the four contexts a frontend saves in.
 *
 * It goes through the shipped library, so what it exercises is the thing a
 * frontend loads rather than a pile of this tree's objects.
 */

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

#define ROM(name) FIXTURES_DIR "/" name ".rp6502"

/* Big enough for any blob this core makes; the cases check it is. */
static uint8_t blob[1 << 20];
static uint8_t other[1 << 20];

static void stand_up(int *utest_result)
{
    ASSERT_TRUE(fe_load(ROM("adventure")));
    fe_run(20);
    ASSERT_LE(fe.serialize_size(), sizeof blob);
}

/* A frontend allocates once against this number and then asks a second entry
 * point for it again, so it has to be the same both times and it must not
 * grow while content is loaded. */
UTEST(state, the_size_is_one_number)
{
    size_t before = fe.serialize_size();
    ASSERT_GT(before, (size_t)0);
    stand_up(utest_result);
    if (*utest_result)
        return;
    ASSERT_EQ(fe.serialize_size(), before);
    fe_run(60);
    ASSERT_EQ(fe.serialize_size(), before);
    fe.unload_game();
    ASSERT_EQ(fe.serialize_size(), before);
}

/* The whole point: a program is running, is written down, is run on, is put
 * back, and picks up from where the blob says rather than from where it got
 * to. The picture is the witness, because it is the one thing no row touches
 * -- it comes back only because the machine that draws it did. */
UTEST(state, a_program_comes_back_to_where_it_was)
{
    stand_up(utest_result);
    if (*utest_result)
        return;
    ASSERT_TRUE(fe.serialize(blob, fe.serialize_size()));

    fe_run(1);
    uint32_t at_save[640 * 480];
    memcpy(at_save, fe.frame_copy, sizeof at_save);

    /* Somewhere else entirely. */
    fe_run(90);
    ASSERT_NE(memcmp(at_save, fe.frame_copy, sizeof at_save), 0);

    ASSERT_TRUE(fe.unserialize(blob, fe.serialize_size()));
    fe_run(1);
    ASSERT_EQ(memcmp(at_save, fe.frame_copy, sizeof at_save), 0);
    fe.unload_game();
}

/* Two blobs of one unchanged machine are the same bytes. Netplay CRCs the
 * whole payload on a check frame, so a byte that is this host's rather than
 * this machine's disables desync detection for the session. */
UTEST(state, two_saves_of_one_machine_agree)
{
    stand_up(utest_result);
    if (*utest_result)
        return;
    size_t n = fe.serialize_size();
    ASSERT_TRUE(fe.serialize(blob, n));
    ASSERT_TRUE(fe.serialize(other, n));
    ASSERT_EQ(memcmp(blob, other, n), 0);
    fe.unload_game();
}

/* A blob the core refuses leaves a core that still plays. That is the whole
 * reason the load takes a copy of the machine before it touches it. */
UTEST(state, a_refused_blob_leaves_a_working_core)
{
    stand_up(utest_result);
    if (*utest_result)
        return;
    size_t n = fe.serialize_size();
    ASSERT_TRUE(fe.serialize(blob, n));

    memcpy(other, blob, n);
    other[0] ^= 0xFF; /* the magic */
    ASSERT_FALSE(fe.unserialize(other, n));

    memcpy(other, blob, n);
    other[n / 2] ^= 0xFF; /* somewhere in the middle: the payload sum */
    ASSERT_FALSE(fe.unserialize(other, n));

    ASSERT_FALSE(fe.unserialize(blob, n - 1)); /* short of its own total */

    int before = fe.video_calls;
    fe_run(4);
    ASSERT_EQ(fe.video_calls, before + 4);
    ASSERT_TRUE(fe.unserialize(blob, n)); /* and the real one still takes */
    fe.unload_game();
}

/* Sound is the part a frontend notices first. A load that restarted the
 * engines would click once a frame under rewind. */
UTEST(state, the_sound_comes_back)
{
    ASSERT_TRUE(fe_load(ROM("furelise")));
    fe_run(120);
    size_t n = fe.serialize_size();
    ASSERT_LE(n, sizeof blob);
    ASSERT_TRUE(fe.serialize(blob, n));
    fe.audio_peak = 0;
    fe_run(30);
    int went = fe.audio_peak;
    ASSERT_GT(went, 0); /* it really was sounding */

    ASSERT_TRUE(fe.unserialize(blob, n));
    fe.audio_peak = 0;
    fe_run(30);
    ASSERT_GT(fe.audio_peak, 0);
    fe.unload_game();
}

/* Each of the four contexts is answered rather than refused, and the core
 * asks. A frontend that will not answer the experimental call falls through
 * to the deprecated AV bit, and a frontend with neither still gets a blob. */
UTEST(state, every_savestate_context_is_answered)
{
    stand_up(utest_result);
    if (*utest_result)
        return;
    size_t n = fe.serialize_size();
    static const int contexts[] = {
        RETRO_SAVESTATE_CONTEXT_NORMAL,
        RETRO_SAVESTATE_CONTEXT_RUNAHEAD_SAME_INSTANCE,
        RETRO_SAVESTATE_CONTEXT_RUNAHEAD_SAME_BINARY,
        RETRO_SAVESTATE_CONTEXT_ROLLBACK_NETPLAY,
    };
    for (unsigned i = 0; i < 4; i++)
    {
        fe.savestate_context = contexts[i];
        fe.savestate_context_asked = false;
        ASSERT_TRUE(fe.serialize(blob, n));
        ASSERT_TRUE(fe.savestate_context_asked);
        ASSERT_TRUE(fe.unserialize(blob, n));
        fe_run(2);
    }

    /* The call is experimental, so a frontend may simply say no. */
    fe.savestate_context_refused = true;
    fe.av_enable |= RETRO_AV_ENABLE_FAST_SAVESTATES;
    ASSERT_TRUE(fe.serialize(blob, n));
    ASSERT_TRUE(fe.unserialize(blob, n));
    fe.av_enable &= ~RETRO_AV_ENABLE_FAST_SAVESTATES;
    ASSERT_TRUE(fe.serialize(blob, n));
    ASSERT_TRUE(fe.unserialize(blob, n));
    fe.savestate_context_refused = false;
    fe.savestate_context = RETRO_SAVESTATE_CONTEXT_NORMAL;
    fe.unload_game();
}

/* A netplay blob carries no host path, so the two peers' payloads can be
 * compared byte for byte. Same shape, same total, different bytes only where
 * a path would have been. */
UTEST(state, a_shared_blob_is_the_same_shape)
{
    stand_up(utest_result);
    if (*utest_result)
        return;
    size_t n = fe.serialize_size();
    fe.savestate_context = RETRO_SAVESTATE_CONTEXT_NORMAL;
    ASSERT_TRUE(fe.serialize(blob, n));
    fe.savestate_context = RETRO_SAVESTATE_CONTEXT_ROLLBACK_NETPLAY;
    ASSERT_TRUE(fe.serialize(other, n));
    ASSERT_EQ(fe.serialize_size(), n); /* the shape does not move */
    ASSERT_NE(memcmp(blob, other, n), 0); /* but the paths are gone */
    ASSERT_TRUE(fe.unserialize(other, n));
    fe.savestate_context = RETRO_SAVESTATE_CONTEXT_NORMAL;
    fe.unload_game();
}

/* A blob outlives the session that made it: the core is taken down to no
 * content at all and stood back up, and the bytes still mean the machine. */
UTEST(state, a_blob_survives_unload_and_load)
{
    stand_up(utest_result);
    if (*utest_result)
        return;
    size_t n = fe.serialize_size();
    ASSERT_TRUE(fe.serialize(blob, n));
    fe_run(1);
    uint32_t at_save[640 * 480];
    memcpy(at_save, fe.frame_copy, sizeof at_save);

    fe.unload_game();
    ASSERT_TRUE(fe_load(ROM("adventure")));
    fe_run(3);

    ASSERT_TRUE(fe.unserialize(blob, n));
    fe_run(1);
    ASSERT_EQ(memcmp(at_save, fe.frame_copy, sizeof at_save), 0);
    fe.unload_game();
}

/* The quirks word: declared, and declared as nothing. This blob is one fixed
 * size, big-endian, holds no pointer and crosses sessions, so every quirk
 * bit is a claim the core would be making falsely. */
UTEST(state, no_quirks_are_claimed)
{
    fe.serialization_quirks_set = false;
    ASSERT_TRUE(fe_load(ROM("adventure")));
    ASSERT_TRUE(fe.serialization_quirks_set);
    ASSERT_EQ(fe.serialization_quirks, (uint64_t)0);
    fe.unload_game();
}
