/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What retro_load_game answers, and what it leaves behind.
 *
 * A refused image is refused on both machines, and this host is a third
 * place that has to say so: the verdict the loader reaches has to survive
 * being carried out through the ABI as retro_load_game's own answer, with
 * nothing else folded into it.
 */

#include "retro_fe.h"
#include "tb_rom.h"
#include "utest.h"

#include <cstring>
#include <string>
#include <vector>

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    fe_open();
    int rc = utest_main(argc, argv);
    fe_close();
    return rc;
}

static const uint8_t prog_stp[] = {0xDB};
static const uint8_t vectors[] = {0x00, 0x03};

/* Enough of a program to reach STP, so an accepted image is one that ran. */
static std::vector<uint8_t> rom_shell()
{
    std::vector<uint8_t> rom;
    const char magic[] = "#!RP6502\n";
    rom.insert(rom.end(), magic, magic + strlen(magic));
    tb_rom_record(rom, 0x0300, prog_stp, sizeof(prog_stp));
    tb_rom_record(rom, 0xFFFC, vectors, sizeof(vectors));
    return rom;
}

static const char *write_rom(const char *name, const std::vector<uint8_t> &rom)
{
    static std::string path;
    path = std::string(TEST_SCRATCH "/") + name;
    return tb_rom_write(path.c_str(), rom) ? path.c_str() : NULL;
}

UTEST(load, a_program_is_taken)
{
    const char *path = write_rom("good.rp6502", rom_shell());
    ASSERT_TRUE(path != NULL);
    ASSERT_TRUE(fe_load(path));
    fe.unload_game();
}

UTEST(load, a_file_that_is_not_one_is_refused)
{
    std::vector<uint8_t> rom;
    const char junk[] = "this is not a program\n";
    rom.insert(rom.end(), junk, junk + strlen(junk));
    const char *path = write_rom("magic.rp6502", rom);
    ASSERT_TRUE(path != NULL);
    ASSERT_FALSE(fe_load(path));
}

UTEST(load, a_program_with_nowhere_to_start_is_refused)
{
    /* Every record intact, no reset vector: there is no address to run from. */
    std::vector<uint8_t> rom;
    const char magic[] = "#!RP6502\n";
    rom.insert(rom.end(), magic, magic + strlen(magic));
    tb_rom_record(rom, 0x0300, prog_stp, sizeof(prog_stp));
    const char *path = write_rom("novector.rp6502", rom);
    ASSERT_TRUE(path != NULL);
    ASSERT_FALSE(fe_load(path));
}

UTEST(load, a_record_across_the_boundary_is_refused)
{
    uint8_t pat[16];
    memset(pat, 0x5A, sizeof pat);
    std::vector<uint8_t> rom = rom_shell();
    tb_rom_record(rom, 0xFFF8, pat, sizeof(pat)); /* runs past 0xFFFF */
    const char *path = write_rom("straddle.rp6502", rom);
    ASSERT_TRUE(path != NULL);
    ASSERT_FALSE(fe_load(path));
}

UTEST(load, a_file_that_is_not_there_is_refused)
{
    ASSERT_FALSE(fe_load(TEST_SCRATCH "/no_such_program.rp6502"));
}

/* A core that has said no is still a core: the frontend's next choice of
 * content has to run. */
UTEST(load, a_refusal_leaves_a_working_core)
{
    ASSERT_FALSE(fe_load(TEST_SCRATCH "/still_not_there.rp6502"));
    const char *path = write_rom("after_refusal.rp6502", rom_shell());
    ASSERT_TRUE(path != NULL);
    ASSERT_TRUE(fe_load(path));
    fe_run(4);
    ASSERT_TRUE(fe.video_calls > 0);
    fe.unload_game();
}

/* A boot is a fresh machine. What the last program left in XRAM is not
 * something the next one should be able to read. */
UTEST(load, a_boot_is_a_fresh_machine)
{
    uint8_t pat[256];
    for (int i = 0; i < 256; i++)
        pat[i] = (uint8_t)(i ^ 0x3C);
    std::vector<uint8_t> first = rom_shell();
    tb_rom_record(first, 0x10100, pat, sizeof(pat));
    const char *path = write_rom("residue_a.rp6502", first);
    ASSERT_TRUE(path != NULL);
    ASSERT_TRUE(fe_load(path));
    fe_run(4);

    const uint8_t *xram = (const uint8_t *)fe.get_memory_data(RETRO_MEMORY_VIDEO_RAM);
    ASSERT_TRUE(xram != NULL);
    ASSERT_EQ(0, memcmp(xram + 0x0100, pat, sizeof pat)); /* it was there */

    fe.unload_game();
    const char *plain = write_rom("residue_b.rp6502", rom_shell());
    ASSERT_TRUE(plain != NULL);
    ASSERT_TRUE(fe_load(plain));
    fe_run(4);

    /* The second program wrote nothing there, so nothing should be there. */
    xram = (const uint8_t *)fe.get_memory_data(RETRO_MEMORY_VIDEO_RAM);
    ASSERT_NE(0, memcmp(xram + 0x0100, pat, sizeof pat));
    fe.unload_game();
}

/* Reset is a reboot: the image is laid down again over a machine that has
 * been wiped. Asked of memory the loader wrote and something else then
 * changed, because a picture that would settle the same way whether or not
 * anything happened is a claim that cannot fail. */
UTEST(load, reset_is_a_reboot)
{
    uint8_t pat[256];
    for (int i = 0; i < 256; i++)
        pat[i] = (uint8_t)(i ^ 0x5A);
    std::vector<uint8_t> rom = rom_shell();
    tb_rom_record(rom, 0x10200, pat, sizeof(pat));
    const char *path = write_rom("reset.rp6502", rom);
    ASSERT_TRUE(path != NULL);
    ASSERT_TRUE(fe_load(path));
    fe_run(20);

    uint8_t *xram = (uint8_t *)fe.get_memory_data(RETRO_MEMORY_VIDEO_RAM);
    ASSERT_TRUE(xram != NULL);
    ASSERT_EQ(0, memcmp(xram + 0x0200, pat, sizeof pat));

    /* Scribble over what the loader put there ... */
    memset(xram + 0x0200, 0xC3, sizeof pat);
    ASSERT_NE(0, memcmp(xram + 0x0200, pat, sizeof pat));

    /* ... and the reset lays the image down again. */
    fe.reset();
    fe_run(20);
    xram = (uint8_t *)fe.get_memory_data(RETRO_MEMORY_VIDEO_RAM);
    ASSERT_EQ(0, memcmp(xram + 0x0200, pat, sizeof pat));
    fe.unload_game();
}

/* And it settles to the same picture a fresh load of it settles to. The
 * machine is compared with itself — no other machine says what that
 * picture is. */
UTEST(load, reset_settles_where_a_fresh_load_does)
{
    const char *path = write_rom("reset2.rp6502", rom_shell());
    ASSERT_TRUE(path != NULL);
    ASSERT_TRUE(fe_load(path));
    fe_run(20);
    static uint32_t fresh[640 * 480];
    const unsigned w = fe.frame_w, h = fe.frame_h;
    memcpy(fresh, fe.frame_copy, (size_t)w * h * sizeof(uint32_t));

    fe.reset();
    fe_run(20);
    ASSERT_EQ(w, fe.frame_w);
    ASSERT_EQ(h, fe.frame_h);
    ASSERT_EQ(0, memcmp(fresh, fe.frame_copy, (size_t)w * h * sizeof(uint32_t)));
    fe.unload_game();
}
