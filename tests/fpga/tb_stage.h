/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The staging store, as the platform will present it: the picked ROM at
 * the bottom and the font asset in the last 64 KB, both read a byte at a
 * time through the same window. The Pocket fills it from two data slots
 * before the core comes out of reset; here a test's clock loop asks this
 * for whatever byte the machine reached for.
 *
 * The asset is the core's own file, not a fixture, so it loads once and
 * every test shares it.
 */

#ifndef _TESTS_FPGA_TB_STAGE_H_
#define _TESTS_FPGA_TB_STAGE_H_

#include <cstdint>
#include <cstdio>
#include <vector>

/* The last 64 KB of the Pocket's 64 MB, above any ROM the loader will
 * ever be handed. Matches FONTS in the firmware's mmio.h. */
#define TB_STAGE_FONT_BASE 0x03FF0000u
/* And the 64 KB below the transfer scratch, holding the code page
 * tables. Matches OEMCP in the firmware's mmio.h. */
#define TB_STAGE_OEMCP_BASE 0x03FD0000u

static const std::vector<uint8_t> &tb_stage_fonts()
{
    static std::vector<uint8_t> fonts = [] {
        std::vector<uint8_t> v;
        FILE *f = fopen(FONTS_BIN, "rb");
        if (!f)
            return v;
        uint8_t buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
            v.insert(v.end(), buf, buf + n);
        fclose(f);
        return v;
    }();
    return fonts;
}

static const std::vector<uint8_t> &tb_stage_oemcp()
{
    static std::vector<uint8_t> t = [] {
        std::vector<uint8_t> v;
        FILE *f = fopen(OEMCP_BIN, "rb");
        if (!f)
            return v;
        uint8_t buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
            v.insert(v.end(), buf, buf + n);
        fclose(f);
        return v;
    }();
    return t;
}

static uint8_t tb_stage(const std::vector<uint8_t> &rom, uint32_t addr)
{
    if (addr >= TB_STAGE_FONT_BASE)
    {
        const std::vector<uint8_t> &fonts = tb_stage_fonts();
        uint32_t i = addr - TB_STAGE_FONT_BASE;
        return i < fonts.size() ? fonts[i] : 0;
    }
    if (addr >= TB_STAGE_OEMCP_BASE)
    {
        const std::vector<uint8_t> &t = tb_stage_oemcp();
        uint32_t i = addr - TB_STAGE_OEMCP_BASE;
        return i < t.size() ? t[i] : 0;
    }
    return addr < rom.size() ? rom[addr] : 0;
}

#endif /* _TESTS_FPGA_TB_STAGE_H_ */
