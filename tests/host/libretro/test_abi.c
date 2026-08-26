/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What the library offers, and what it does not.
 *
 * The ABI is the whole of a core's surface. That is a claim about the file
 * on disk — which symbols a frontend can reach in it — so it is asked of the
 * file on disk, through the same loader a frontend uses.
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

UTEST(abi, version_is_one)
{
    ASSERT_EQ(fe.api_version(), (unsigned)RETRO_API_VERSION);
    ASSERT_EQ(fe.api_version(), 1u);
}

/* fe_open resolves every entry point and exits if one is missing, so reaching
 * a case at all is most of this claim. This says the rest: the machine the
 * core is made of stays inside it. */
UTEST(abi, the_machine_is_not_exported)
{
    static const char *inside[] = {
        "main_init", "main_run", "main_stop", "rom_load", "sys_run_frame",
        "vga_set_framebuffer", "vga_canvas_size", "com_set_tx_tap", "mem_init",
        "aud_read", "aud_pump", "ram", "xram", "regs", "xstack",
        "keyboard_hid_set", "gamepad_host_report", "fs_open", "fs_read", "log_error",
    };
    for (size_t i = 0; i < sizeof inside / sizeof *inside; i++)
    {
        void *sym = fe_dl_sym(fe.lib, inside[i]);
        ASSERT_TRUE_MSG(sym == NULL, inside[i]);
    }
}

UTEST(abi, system_info_names_the_program_it_runs)
{
    struct retro_system_info info;
    fe.get_system_info(&info);
    ASSERT_TRUE(info.library_name && info.library_name[0]);
    ASSERT_TRUE(info.library_version && info.library_version[0]);
    /* The frontend prints the word "Version" itself. */
    ASSERT_TRUE(strncmp(info.library_version, "Version ", 8) != 0);
    ASSERT_TRUE(info.valid_extensions && strstr(info.valid_extensions, "rp6502"));
    /* A program's assets are read from the file while it runs, so the file
     * has to still be there: the frontend must not hand us bytes instead. */
    ASSERT_TRUE(info.need_fullpath);
}

/* There is no monitor on this host. A frontend that offers to start the core
 * with nothing would be offering an empty screen. */
UTEST(abi, there_is_nothing_to_run_without_a_program)
{
    ASSERT_FALSE(fe.supports_no_game);
    ASSERT_FALSE(fe.load_game(NULL));
}

/* Answering zero is how a core says it has no savestates. Answering anything
 * else promises rewind and netplay it cannot keep. */
UTEST(abi, savestates_are_declined_rather_than_faked)
{
    ASSERT_EQ(fe.serialize_size(), (size_t)0);
    char buf[64];
    ASSERT_FALSE(fe.serialize(buf, sizeof buf));
    ASSERT_FALSE(fe.unserialize(buf, sizeof buf));
}

UTEST(abi, av_info_is_the_machine_it_emulates)
{
    struct retro_system_av_info av;
    fe.get_system_av_info(&av);
    /* The largest canvas, which is the boot console. */
    ASSERT_EQ(av.geometry.max_width, 640u);
    ASSERT_EQ(av.geometry.max_height, 480u);
    /* The RP6502's VGA is always 60 Hz, and its audio is generated at 48k. */
    ASSERT_EQ((int)av.timing.fps, 60);
    ASSERT_EQ((int)av.timing.sample_rate, 48000);
}

UTEST(abi, the_memory_a_frontend_may_read)
{
    ASSERT_TRUE(fe.get_memory_data(RETRO_MEMORY_SYSTEM_RAM) != NULL);
    ASSERT_EQ(fe.get_memory_size(RETRO_MEMORY_SYSTEM_RAM), (size_t)0x10000);
    ASSERT_TRUE(fe.get_memory_data(RETRO_MEMORY_VIDEO_RAM) != NULL);
    ASSERT_EQ(fe.get_memory_size(RETRO_MEMORY_VIDEO_RAM), (size_t)0x10000);
    /* Nothing else is on offer. */
    ASSERT_TRUE(fe.get_memory_data(RETRO_MEMORY_SAVE_RAM) == NULL);
    ASSERT_EQ(fe.get_memory_size(RETRO_MEMORY_SAVE_RAM), (size_t)0);
}

UTEST(abi, the_options_are_declared_before_anything_runs)
{
    ASSERT_TRUE(fe.options_declared);
    ASSERT_TRUE(fe.option_count > 0);
}
