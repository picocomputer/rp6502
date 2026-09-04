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
 * The CRC is host_crc32's: the bench compiles core/sys/crc32.c beside
 * tb_seed.c, and tests/host/emu/test_units.c holds it to the standard
 * vectors.
 *
 * tests/gen/rp6502_rom.py is this file in Python, for the generators that
 * write images to disk. The two must agree on the header format; there
 * is one format, and tests/rtl/ria/test_rom.cpp is where it is asserted.
 */

#ifndef _TESTS_BENCH_TB_ROM_H_
#define _TESTS_BENCH_TB_ROM_H_

#include "host/host.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

/* The address is five digits so one format serves both the 6502's
 * sixteen bits and XRAM's seventeen; the loader scans hex and does not
 * care about the leading zero. Chunked at the format's 1024-byte record
 * cap, as the packer writes them -- the loaders refuse anything bigger.
 * Deliberately NOT split at the 64 KB page: the straddle rule is the
 * record parser's to refuse, and the suites that prove it write the
 * straddle through here. */
static void tb_rom_record(std::vector<uint8_t> &rom, uint32_t addr,
                          const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    while (len)
    {
        size_t n = len < 1024 ? len : 1024;
        char line[64];
        snprintf(line, sizeof(line), "$%05X $%zX $%08X\n",
                 addr, n, host_crc32(0, p, n));
        rom.insert(rom.end(), line, line + strlen(line));
        rom.insert(rom.end(), p, p + n);
        addr += (uint32_t)n;
        p += n;
        len -= n;
    }
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
