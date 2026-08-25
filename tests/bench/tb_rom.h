/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The .rp6502 container, as the loader reads it: a magic line, then a
 * record per block — an ASCII header naming the address, the length and
 * a CRC, followed by the bytes. A runnable image ends with the reset
 * vector, which is a record like any other.
 *
 * The CRC is spelled out rather than taken from mem_crc32 because a
 * bench builds images for tests that link no emulator, and a table this
 * short is cheaper than the dependency. tests/host/emu/test_units.c holds
 * mem_crc32 to the same vectors.
 *
 * tests/gen/rp6502_rom.py is this file in Python, for the generators that
 * write images to disk. The two must agree on the header format; there
 * is one format, and tests/rtl/ria/test_rom.cpp is where it is asserted.
 */

#ifndef _TESTS_BENCH_TB_ROM_H_
#define _TESTS_BENCH_TB_ROM_H_

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static uint32_t tb_rom_crc32(const void *buf, size_t n)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t crc = 0xFFFFFFFFu;
    while (n--)
    {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return crc ^ 0xFFFFFFFFu;
}

/* The address is five digits so one format serves both the 6502's
 * sixteen bits and XRAM's seventeen; the loader scans hex and does not
 * care about the leading zero. */
static void tb_rom_record(std::vector<uint8_t> &rom, uint32_t addr,
                          const void *data, size_t len)
{
    char line[64];
    snprintf(line, sizeof(line), "$%05X $%zX $%08X\n",
             addr, len, tb_rom_crc32(data, len));
    rom.insert(rom.end(), line, line + strlen(line));
    const uint8_t *p = (const uint8_t *)data;
    rom.insert(rom.end(), p, p + len);
}

static void tb_rom_magic(std::vector<uint8_t> &rom)
{
    static const char magic[] = "#!RP6502\n";
    rom.insert(rom.end(), magic, magic + strlen(magic));
}

/* Where the 6502 starts, which every image has to say. */
static void tb_rom_reset(std::vector<uint8_t> &rom, uint16_t org)
{
    const uint8_t vec[2] = {(uint8_t)org, (uint8_t)(org >> 8)};
    tb_rom_record(rom, 0xFFFC, vec, sizeof(vec));
}

/* A whole runnable image. Extra records — XRAM blocks, a second load
 * address — go on with tb_rom_record before the reset vector, which is
 * why that is not folded in here. */
static std::vector<uint8_t> tb_rom_image(uint16_t org, const void *prog,
                                         size_t len)
{
    std::vector<uint8_t> rom;
    tb_rom_magic(rom);
    tb_rom_record(rom, org, prog, len);
    tb_rom_reset(rom, org);
    return rom;
}

static std::vector<uint8_t> tb_rom_image(uint16_t org,
                                         const std::vector<uint8_t> &prog)
{
    return tb_rom_image(org, prog.data(), prog.size());
}

/* An image on disk, for a suite that assembles its program and then boots it
 * through mut_boot, which takes a path. */
static bool tb_rom_write(const char *path, const std::vector<uint8_t> &rom)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    bool ok = fwrite(rom.data(), 1, rom.size(), f) == rom.size();
    return fclose(f) == 0 && ok;
}

static bool tb_rom_read(const char *path, std::vector<uint8_t> &rom)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        rom.insert(rom.end(), buf, buf + n);
    fclose(f);
    return !rom.empty();
}

#endif /* _TESTS_BENCH_TB_ROM_H_ */
