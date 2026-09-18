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
    /* Each case opens the core itself, because the core queries the
     * frontend's core options version in retro_set_environment and whether
     * it offers input bitmasks in retro_init, and the cases differ in both. */
    return utest_main(argc, argv);
}

static bool has_option(const char *key)
{
    for (int i = 0; i < fe.option_count; i++)
        if (!strcmp(fe.option_key[i], key))
            return true;
    return false;
}

UTEST(options, a_current_frontend_is_given_the_new_form)
{
    fe_open_as(2, true);
    ASSERT_TRUE(fe.options_declared);
    ASSERT_FALSE(fe.variables_declared);
    ASSERT_TRUE(fe.option_count > 0);
    ASSERT_TRUE(has_option("rp6502_phi2"));
    ASSERT_TRUE(has_option("rp6502_code_page"));
    ASSERT_TRUE(has_option("rp6502_mem_fill"));
    fe_close();
}

UTEST(options, an_old_frontend_is_given_the_old_form)
{
    fe_open_as(0, false);
    ASSERT_TRUE(fe.variables_declared);
    ASSERT_FALSE(fe.options_declared);
    ASSERT_TRUE(has_option("rp6502_phi2"));
    ASSERT_TRUE(has_option("rp6502_code_page"));
    ASSERT_TRUE(has_option("rp6502_mem_fill"));
    fe_close();
}

UTEST(options, the_old_form_carries_a_label_and_its_values)
{
    fe_open_as(0, false);
    for (int i = 0; i < fe.option_count; i++)
    {
        const char *text = fe.option_text[i];
        const char *semi = strchr(text, ';');
        ASSERT_TRUE_MSG(semi != NULL, fe.option_key[i]);
        ASSERT_TRUE_MSG(semi != text, fe.option_key[i]);
        ASSERT_TRUE_MSG(strchr(semi, '|') != NULL, fe.option_key[i]);
        if (!strcmp(fe.option_key[i], "rp6502_mem_fill"))
            ASSERT_STREQ("Memory At Power-On; random|00|ff", text);
    }
    fe_close();
}

UTEST(options, they_are_read_before_the_program_starts)
{
    fe_open_as(2, true);
    for (int i = 0; i < fe.option_count; i++)
        fe.option_value[i] = NULL;
    int before = fe.get_variable_calls;
    ASSERT_TRUE(fe_load(ROMS_DIR "/mode3_8bpp.rp6502"));
    ASSERT_TRUE(fe.get_variable_calls > before);
    ASSERT_EQ(fe.video_calls, 0);
    fe.unload_game();
    fe_close();
}

UTEST(options, a_value_that_makes_no_sense_is_survived)
{
    fe_open_as(2, true);
    for (int i = 0; i < fe.option_count; i++)
        fe.option_value[i] = "wharrgarbl";
    ASSERT_TRUE(fe_load(ROMS_DIR "/mode3_8bpp.rp6502"));
    fe_run(20);
    ASSERT_TRUE(fe.video_calls > 0);
    fe.unload_game();
    fe_close();
}

UTEST(options, a_change_is_noticed_while_running)
{
    fe_open_as(2, true);
    for (int i = 0; i < fe.option_count; i++)
        fe.option_value[i] = NULL;
    ASSERT_TRUE(fe_load(ROMS_DIR "/mode3_8bpp.rp6502"));
    fe_run(10);

    for (int i = 0; i < fe.option_count; i++)
        if (!strcmp(fe.option_key[i], "rp6502_phi2"))
            fe.option_value[i] = "1000";
    fe.variables_dirty = true;
    int before = fe.get_variable_calls;
    fe_run(2);
    ASSERT_TRUE(fe.get_variable_calls > before);
    fe.unload_game();
    fe_close();
}

UTEST(options, a_setting_changed_between_programs_is_taken)
{
    fe_open_as(2, true);
    for (int i = 0; i < fe.option_count; i++)
        fe.option_value[i] = NULL;
    ASSERT_TRUE(fe_load(ROMS_DIR "/mode3_8bpp.rp6502"));
    fe_run(10);
    fe.unload_game();

    for (int i = 0; i < fe.option_count; i++)
        if (!strcmp(fe.option_key[i], "rp6502_mem_fill"))
            fe.option_value[i] = "ff";
    ASSERT_TRUE(fe_load(ROMS_DIR "/mode3_8bpp.rp6502"));
    fe_run(10);

    const uint8_t *xram = (const uint8_t *)fe.get_memory_data(RETRO_MEMORY_VIDEO_RAM);
    ASSERT_TRUE(xram != NULL);
    /* mode3_8bpp writes nothing at the top of XRAM, so xram[0xFFFF] still
     * holds the memory fill. */
    ASSERT_EQ(xram[0xFFFF], 0xFF);
    fe.unload_game();
    fe_close();
}

UTEST(options, a_frontend_without_bitmasks_still_has_gamepads)
{
    fe_open_as(2, false);
    ASSERT_TRUE(fe.asked_for_bitmasks);
    ASSERT_TRUE(fe_load(GAMEPAD_ROM));
    fe_run(40);
    fe.input[0][0][RETRO_DEVICE_ID_JOYPAD_START] = 1;
    fe_run(20);
    const uint8_t *xram = (const uint8_t *)fe.get_memory_data(RETRO_MEMORY_VIDEO_RAM);
    ASSERT_TRUE(xram != NULL);
    ASSERT_EQ(xram[0xFF00 + 3], 0x08);
    fe.unload_game();
    fe_close();
}
