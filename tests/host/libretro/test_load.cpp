/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "retro_fe.h"
#include "tb_asm.h"
#include "tb_rom.h"
#include "utest.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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
    tb_rom_record(rom, 0xFFF8, pat, sizeof(pat));
    const char *path = write_rom("straddle.rp6502", rom);
    ASSERT_TRUE(path != NULL);
    ASSERT_FALSE(fe_load(path));
}

UTEST(load, a_file_that_is_not_there_is_refused)
{
    ASSERT_FALSE(fe_load(TEST_SCRATCH "/no_such_program.rp6502"));
}

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
    ASSERT_EQ(0, memcmp(xram + 0x0100, pat, sizeof pat));

    fe.unload_game();
    const char *plain = write_rom("residue_b.rp6502", rom_shell());
    ASSERT_TRUE(plain != NULL);
    ASSERT_TRUE(fe_load(plain));
    fe_run(4);

    xram = (const uint8_t *)fe.get_memory_data(RETRO_MEMORY_VIDEO_RAM);
    ASSERT_NE(0, memcmp(xram + 0x0100, pat, sizeof pat));
    fe.unload_game();
}

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

    memset(xram + 0x0200, 0xC3, sizeof pat);
    ASSERT_NE(0, memcmp(xram + 0x0200, pat, sizeof pat));

    fe.reset();
    fe_run(20);
    xram = (uint8_t *)fe.get_memory_data(RETRO_MEMORY_VIDEO_RAM);
    ASSERT_EQ(0, memcmp(xram + 0x0200, pat, sizeof pat));
    fe.unload_game();
}

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

/* Copies the argv block, an offset table and then argv[0], to XRAM $0100. */
static std::vector<uint8_t> argv_rom()
{
    tb_asm a;
    a.call(0x08); /* ARGV */
    a.store(TB_RW0_ADDR, 0x00);
    a.store(TB_RW0_ADDR + 1, 0x01);
    a.ldx(0);
    uint16_t top = a.here();
    a.lda_abs(TB_XSTACK);
    a.sta(TB_RW0_DATA);
    a.inx();
    a.raw({0xD0, (uint8_t)(top - (a.here() + 2))}); /* bne top */
    a.stp();
    return tb_rom_image(TB_ORG, a.b);
}

static std::string argv0_in_xram()
{
    const uint8_t *xram = (const uint8_t *)fe.get_memory_data(RETRO_MEMORY_VIDEO_RAM);
    if (!xram)
        return "";
    const uint8_t *argv = xram + 0x0100;
    unsigned at = argv[0] | (argv[1] << 8);
    if (at >= 0x100)
        return "";
    const char *argv0 = (const char *)argv + at;
    return std::string(argv0, strnlen(argv0, 0x100 - at));
}

/* Writes "ok" to SAVE:fe.sav. */
static std::vector<uint8_t> save_rom()
{
    tb_asm a;
    a.push_str("SAVE:fe.sav");
    a.call_a(0x14, 0x02 | 0x10 | 0x20); /* OPEN, O_WRONLY | O_CREAT | O_TRUNC */
    a.sta(0x0200);
    a.push('k');
    a.push('o');
    a.lda_abs(0x0200);
    a.sta(TB_API_A);
    a.call(0x18); /* WRITE_XSTACK */
    a.lda_abs(0x0200);
    a.sta(TB_API_A);
    a.call(0x15); /* CLOSE */
    a.stp();
    return tb_rom_image(TB_ORG, a.b);
}

/* NULL is a frontend that has no save folder to give. */
static void fe_save_dir(const char *dir)
{
    fe.have_save_dir = dir != NULL;
    snprintf(fe.save_dir, sizeof fe.save_dir, "%s", dir ? dir : "");
}

static std::string file_text(const std::filesystem::path &p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

UTEST(load, argv0_is_the_absolute_path_with_its_drive)
{
    const char *path = write_rom("argv0.rp6502", argv_rom());
    ASSERT_TRUE(path != NULL);
    ASSERT_TRUE(fe_load(path));
    fe_run(10);
#ifdef _WIN32
    std::string want = std::filesystem::absolute(path).generic_string();
#else
    std::string want = "FS:" + std::filesystem::canonical(path).string();
#endif
    std::string got = argv0_in_xram();
    ASSERT_STREQ(got.c_str(), want.c_str());
    fe.unload_game();
}

/* U+65E5 U+672C, which no single-byte code page holds, name the folder, so
 * the program can only be named as an install. */
UTEST(load, a_path_the_code_page_cannot_hold_boots_as_an_install)
{
    std::filesystem::path dir =
        std::filesystem::u8path(TEST_SCRATCH "/\xE6\x97\xA5\xE6\x9C\xAC");
    std::filesystem::create_directories(dir);
    std::filesystem::path rom = dir / "argv1.rp6502";
    std::vector<uint8_t> image = argv_rom();
    {
        std::ofstream out(rom, std::ios::binary);
        out.write((const char *)image.data(), (std::streamsize)image.size());
        ASSERT_TRUE((bool)out);
    }
    ASSERT_TRUE(fe_load(rom.u8string().c_str()));
    fe_run(10);
    std::string got = argv0_in_xram();
    ASSERT_STREQ(got.c_str(), ":argv1.rp6502");
    fe.unload_game();
}

UTEST(load, the_core_leaves_the_working_directory_alone)
{
    std::filesystem::path before = std::filesystem::current_path();
    std::filesystem::path saves = std::filesystem::path(TEST_SCRATCH) / "cwd_saves";
    std::filesystem::path away = std::filesystem::path(TEST_SCRATCH) / "away";
    std::filesystem::create_directories(saves);
    std::filesystem::create_directories(away);
    const char *path = write_rom("cwd.rp6502", rom_shell());
    ASSERT_TRUE(path != NULL);
    std::filesystem::current_path(away);
    std::filesystem::path here = std::filesystem::current_path();

    for (int with_saves = 0; with_saves < 2; with_saves++)
    {
        fe_save_dir(with_saves ? saves.string().c_str() : NULL);
        ASSERT_TRUE(fe_load(path));
        fe_run(4);
        ASSERT_TRUE(std::filesystem::current_path() == here);
        fe.reset();
        fe_run(4);
        ASSERT_TRUE(std::filesystem::current_path() == here);
        fe.unload_game();
    }
    fe_save_dir(NULL);
    std::filesystem::current_path(before);
}

/* The folder is made by the first save, so a program that never saves leaves
 * nothing behind. */
UTEST(load, a_save_goes_to_rp6502_in_the_frontend_save_folder)
{
    std::filesystem::path saves = std::filesystem::path(TEST_SCRATCH) / "fe_saves";
    std::filesystem::remove_all(saves);
    fe_save_dir(saves.string().c_str());

    const char *quiet = write_rom("quiet.rp6502", rom_shell());
    ASSERT_TRUE(quiet != NULL);
    ASSERT_TRUE(fe_load(quiet));
    fe_run(4);
    fe.unload_game();
    ASSERT_FALSE(std::filesystem::exists(saves));

    const char *saver = write_rom("saver.rp6502", save_rom());
    ASSERT_TRUE(saver != NULL);
    ASSERT_TRUE(fe_load(saver));
    fe_run(10);
    fe.unload_game();
    fe_save_dir(NULL);
    ASSERT_STREQ(file_text(saves / "rp6502" / "fe.sav").c_str(), "ok");
}

UTEST(load, with_no_frontend_save_folder_a_save_goes_to_the_working_directory)
{
    std::filesystem::path before = std::filesystem::current_path();
    std::filesystem::path here = std::filesystem::path(TEST_SCRATCH) / "cwd_is_save";
    std::filesystem::remove_all(here);
    std::filesystem::create_directories(here);
    const char *saver = write_rom("saver.rp6502", save_rom());
    ASSERT_TRUE(saver != NULL);
    std::filesystem::current_path(here);
    fe_save_dir(NULL);
    ASSERT_TRUE(fe_load(saver));
    fe_run(10);
    fe.unload_game();
    std::filesystem::current_path(before);
    ASSERT_STREQ(file_text(here / "fe.sav").c_str(), "ok");
}
