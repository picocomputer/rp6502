/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The whole output's volume and balance: the user's, the program's through
 * its xreg, their sum, and the bell, which only the user's reaches. Heard
 * through aud_render as a sink on the machine's thread takes it. */

#include "core/api/xreg.h"
#include "core/aud/bel.h"
#include "core/aud/mix.h"
#include "core/aud/vol.h"
#include "core/hid/usage.h"
#include "core/str/str.h"
#include "core/sys/config.h"
#include "emu_boot.h"

#include <math.h>

#define FRAME 800
#define LEVEL 16000

static float out[FRAME * 2];

static void probe(int16_t *l, int16_t *r)
{
    *l = *r = LEVEL;
}

/* Each side's level once the resampler has settled on a steady input: the
 * last sample of the last of a few frames. */
static void level(double *l, double *r)
{
    for (int i = 0; i < 4; i++)
    {
        emu_frames(1);
        aud_render(out, FRAME);
    }
    *l = out[(FRAME - 1) * 2] * 32768.0;
    *r = out[(FRAME - 1) * 2 + 1] * 32768.0;
}

/* The loudest sample of a few frames, either side. */
static double peak(int frames)
{
    double p = 0;
    for (int f = 0; f < frames; f++)
    {
        emu_frames(1);
        aud_render(out, FRAME);
        for (int i = 0; i < FRAME * 2; i++)
            if (fabs(out[i]) > p)
                p = fabs(out[i]);
    }
    return p;
}

static void fresh(void)
{
    aud_set_enabled(true);
    vol_set_volume(0);
    vol_set_balance(0);
    vol_stop();
    bel_init();
    aud_setup_probe(probe);
}

static bool near(double got, double want)
{
    return fabs(got - want) <= 2.0 + fabs(want) * 0.002;
}

static double expect(int db)
{
    return LEVEL * (double)vol_gain[-db] / 32768.0;
}

UTEST(volume, the_gain_table_falls_a_db_a_step)
{
    ASSERT_EQ(vol_gain[0], AUD_GAIN_UNITY);
    ASSERT_EQ(vol_gain[-VOL_DB_MUTE], 0);
    for (int db = 0; db < -VOL_DB_MUTE; db++)
        ASSERT_LT(fabs(vol_gain[db] - 32768.0 * pow(10.0, -db / 20.0)), 0.51);
}

UTEST(volume, unity_leaves_the_sound_as_it_was)
{
    fresh();
    double l, r;
    level(&l, &r);
    ASSERT_TRUE(near(l, LEVEL));
    ASSERT_TRUE(near(r, LEVEL));
}

UTEST(volume, the_users_and_the_programs_add)
{
    fresh();
    ASSERT_TRUE(vol_set_volume(-10));
    ASSERT_TRUE(xreg0(1, 2, (uint16_t)-6));
    double l, r;
    level(&l, &r);
    ASSERT_TRUE(near(l, expect(-16)));
    ASSERT_TRUE(near(r, expect(-16)));
    ASSERT_TRUE(xreg0(1, 2, (uint16_t)-60));
    level(&l, &r);
    ASSERT_EQ(l, 0.0);
    ASSERT_EQ(r, 0.0);
}

UTEST(volume, a_positive_balance_turns_the_left_down)
{
    fresh();
    ASSERT_TRUE(vol_set_balance(9));
    double l, r;
    level(&l, &r);
    ASSERT_TRUE(near(l, expect(-9)));
    ASSERT_TRUE(near(r, LEVEL));
    ASSERT_TRUE(xreg0(1, 3, (uint16_t)-12));
    level(&l, &r);
    ASSERT_TRUE(near(l, LEVEL));
    ASSERT_TRUE(near(r, expect(-3)));
}

UTEST(volume, out_of_range_is_refused_and_changes_nothing)
{
    fresh();
    ASSERT_FALSE(vol_set_volume(1));
    ASSERT_FALSE(vol_set_volume(-65));
    ASSERT_FALSE(vol_set_balance(64));
    ASSERT_FALSE(vol_set_balance(-64));
    ASSERT_FALSE(xreg0(1, 2, 1));
    ASSERT_FALSE(xreg0(1, 2, (uint16_t)-65));
    ASSERT_FALSE(xreg0(1, 3, 64));
    ASSERT_FALSE(xreg0(1, 3, (uint16_t)-64));
    double l, r;
    level(&l, &r);
    ASSERT_TRUE(near(l, LEVEL));
    ASSERT_TRUE(near(r, LEVEL));
}

UTEST(volume, a_stop_gives_back_the_programs_but_keeps_the_users)
{
    fresh();
    ASSERT_TRUE(vol_set_volume(-6));
    ASSERT_TRUE(xreg0(1, 2, (uint16_t)-20));
    ASSERT_TRUE(xreg0(1, 3, 20));
    vol_stop();
    double l, r;
    level(&l, &r);
    ASSERT_TRUE(near(l, expect(-6)));
    ASSERT_TRUE(near(r, expect(-6)));
}

UTEST(volume, the_bell_hears_only_the_user)
{
    fresh();
    aud_setup_probe(NULL);
    ASSERT_TRUE(xreg0(1, 2, (uint16_t)VOL_DB_MUTE));
    bel_add(&bel_teletype);
    ASSERT_GT(peak(6), 0.01);
    bel_init();
    ASSERT_TRUE(vol_set_volume(VOL_DB_MUTE));
    bel_add(&bel_teletype);
    /* The first frame still carries the resampler's history of the loud
     * one. */
    peak(1);
    ASSERT_EQ(peak(6), 0.0);
    bel_init();
    vol_stop();
    vol_set_volume(0);
}

UTEST(volume, the_keys_step_and_mute)
{
    fresh();
    vol_key(HID_KEY_VOLUME_DOWN);
    vol_key(HID_KEY_VOLUME_DOWN);
    double l, r;
    level(&l, &r);
    ASSERT_TRUE(near(l, expect(-2 * VOL_KEY_STEP)));
    vol_key(HID_KEY_MUTE);
    level(&l, &r);
    ASSERT_EQ(l, 0.0);
    vol_key(HID_KEY_VOLUME_UP);
    level(&l, &r);
    ASSERT_TRUE(near(l, expect(-2 * VOL_KEY_STEP)));
    vol_key(HID_KEY_VOLUME_UP);
    vol_key(HID_KEY_VOLUME_UP);
    vol_key(HID_KEY_VOLUME_UP);
    level(&l, &r);
    ASSERT_TRUE(near(l, LEVEL));
    vol_key(HID_KEY_A);
    level(&l, &r);
    ASSERT_TRUE(near(l, LEVEL));
}

UTEST(volume, a_signed_setting_parses_and_round_trips)
{
    const char *s = "-12";
    int32_t v;
    ASSERT_TRUE(str_parse_int32(&s, &v));
    ASSERT_EQ(v, -12);
    s = "-2147483648";
    ASSERT_TRUE(str_parse_int32(&s, &v));
    ASSERT_EQ(v, INT32_MIN);
    s = "-2147483649";
    ASSERT_FALSE(str_parse_int32(&s, &v));
    s = "2147483648";
    ASSERT_FALSE(str_parse_int32(&s, &v));
    s = "-";
    ASSERT_FALSE(str_parse_int32(&s, &v));
    config_load_line('G', "-21");
    ASSERT_EQ(vol_get_volume(), -21);
    config_load_line('G', "-65");
    ASSERT_EQ(vol_get_volume(), -21);
    config_load_line('Q', "-7");
    ASSERT_EQ(vol_get_balance(), -7);
    vol_set_volume(0);
    vol_set_balance(0);
}

UTEST_MAIN_EMU()
