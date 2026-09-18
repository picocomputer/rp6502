/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TESTS_BENCH_TB_ROM_H_
#define _TESTS_BENCH_TB_ROM_H_

#include "host/host.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

/* Records are split at 1024 bytes, which is ROM_RECORD_MAX in
 * core/rom/rom.h, because the loader rejects a longer record. */
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

static void tb_rom_reset(std::vector<uint8_t> &rom, uint16_t org)
{
    const uint8_t vec[2] = {(uint8_t)org, (uint8_t)(org >> 8)};
    tb_rom_record(rom, 0xFFFC, vec, sizeof(vec));
}

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
