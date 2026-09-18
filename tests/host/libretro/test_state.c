/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
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


static uint8_t blob[1 << 20];
static uint8_t other[1 << 20];

static void stand_up(int *utest_result)
{
    ASSERT_TRUE(fe_load(ADVENTURE_ROM));
    fe_run(20);
    ASSERT_LE(fe.serialize_size(), sizeof blob);
}

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

UTEST(state, a_program_comes_back_to_where_it_was)
{
    stand_up(utest_result);
    if (*utest_result)
        return;
    ASSERT_TRUE(fe.serialize(blob, fe.serialize_size()));

    fe_run(1);
    static uint32_t at_save[640 * 480];
    memcpy(at_save, fe.frame_copy, sizeof at_save);

    fe_run(90);
    ASSERT_NE(memcmp(at_save, fe.frame_copy, sizeof at_save), 0);

    ASSERT_TRUE(fe.unserialize(blob, fe.serialize_size()));
    fe_run(1);
    ASSERT_EQ(memcmp(at_save, fe.frame_copy, sizeof at_save), 0);
    fe.unload_game();
}

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
    other[n / 2] ^= 0xFF; /* a byte flipped in the body fails the payload sum */
    ASSERT_FALSE(fe.unserialize(other, n));

    ASSERT_FALSE(fe.unserialize(blob, n - 1)); /* n - 1 is less than the total in the header */

    int before = fe.video_calls;
    fe_run(4);
    ASSERT_EQ(fe.video_calls, before + 4);
    ASSERT_TRUE(fe.unserialize(blob, n));
    fe.unload_game();
}

UTEST(state, the_sound_comes_back)
{
    ASSERT_TRUE(fe_load(AUD_ROM_PSG));
    fe_run(20);
    size_t n = fe.serialize_size();
    ASSERT_LE(n, sizeof blob);
    ASSERT_TRUE(fe.serialize(blob, n));
    fe.audio_peak = 0;
    fe_run(30);
    int went = fe.audio_peak;
    ASSERT_GT(went, 0);

    ASSERT_TRUE(fe.unserialize(blob, n));
    fe.audio_peak = 0;
    fe_run(30);
    ASSERT_GT(fe.audio_peak, 0);
    fe.unload_game();
}

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
    ASSERT_EQ(fe.serialize_size(), n);
    ASSERT_NE(memcmp(blob, other, n), 0); /* the path slots are empty under SST_SHARED */
    ASSERT_TRUE(fe.unserialize(other, n));
    fe.savestate_context = RETRO_SAVESTATE_CONTEXT_NORMAL;
    fe.unload_game();
}

UTEST(state, a_blob_survives_unload_and_load)
{
    stand_up(utest_result);
    if (*utest_result)
        return;
    size_t n = fe.serialize_size();
    ASSERT_TRUE(fe.serialize(blob, n));
    fe_run(1);
    static uint32_t at_save[640 * 480];
    memcpy(at_save, fe.frame_copy, sizeof at_save);

    fe.unload_game();
    ASSERT_TRUE(fe_load(ADVENTURE_ROM));
    fe_run(3);

    ASSERT_TRUE(fe.unserialize(blob, n));
    fe_run(1);
    ASSERT_EQ(memcmp(at_save, fe.frame_copy, sizeof at_save), 0);
    fe.unload_game();
}

UTEST(state, no_quirks_are_claimed)
{
    fe.serialization_quirks_set = false;
    ASSERT_TRUE(fe_load(ADVENTURE_ROM));
    ASSERT_TRUE(fe.serialization_quirks_set);
    ASSERT_EQ(fe.serialization_quirks, (uint64_t)0);
    fe.unload_game();
}
