/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The staging store, as the platform presents it. The fonts and the code
 * page tables are whole files the host places once and the machine reads
 * at random. Everything else a slot moves arrives through that slot's own
 * 32 KB window, written by the host when it performs a read — so this is
 * a store and not a function of the ROM: the picked image is a file on
 * the host now, not a copy sitting at address zero.
 *
 * The assets are the core's own files, not fixtures, so they load once
 * and every test shares them.
 */

#ifndef _TESTS_FPGA_TB_STAGE_H_
#define _TESTS_FPGA_TB_STAGE_H_

#include <cstdint>
#include <cstdio>
#include <map>
#include <vector>

/* All six must agree with the firmware's mmio.h and with data.json. */
#define TB_STAGE_FONT_BASE 0x03FEA000u
#define TB_STAGE_FONT_SIZE 0x0000F000u
#define TB_STAGE_OEMCP_BASE 0x03FE8000u
#define TB_STAGE_OEMCP_SIZE 0x00002000u
#define TB_STAGE_WIN_BASE 0x03FA0000u
#define TB_STAGE_WIN_SIZE 0x00008000u

static const std::vector<uint8_t> &tb_stage_load(const char *path,
                                                 std::vector<uint8_t> &v)
{
    if (!v.empty())
        return v;
    FILE *f = fopen(path, "rb");
    if (!f)
        return v;
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        v.insert(v.end(), buf, buf + n);
    fclose(f);
    return v;
}

static const std::vector<uint8_t> &tb_stage_fonts()
{
    static std::vector<uint8_t> v;
    return tb_stage_load(FONTS_BIN, v);
}

static const std::vector<uint8_t> &tb_stage_oemcp()
{
    static std::vector<uint8_t> v;
    return tb_stage_load(OEMCP_BIN, v);
}

/* What the host has written into the windows. Sparse, because a test
 * touches a few kilobytes of a 288 KB span. */
static std::map<uint32_t, uint8_t> &tb_stage_store()
{
    static std::map<uint32_t, uint8_t> m;
    return m;
}

static void tb_stage_write(uint32_t addr, uint8_t b)
{
    tb_stage_store()[addr] = b;
}

static void tb_stage_clear()
{
    tb_stage_store().clear();
}

/* The rom argument is vestigial now that the host model serves the image,
 * and kept so the clock loops that call this need no edit. */
static uint8_t tb_stage(const std::vector<uint8_t> &rom, uint32_t addr)
{
    (void)rom;
    if (addr >= TB_STAGE_FONT_BASE
        && addr < TB_STAGE_FONT_BASE + TB_STAGE_FONT_SIZE)
    {
        const std::vector<uint8_t> &fonts = tb_stage_fonts();
        uint32_t i = addr - TB_STAGE_FONT_BASE;
        return i < fonts.size() ? fonts[i] : 0;
    }
    if (addr >= TB_STAGE_OEMCP_BASE
        && addr < TB_STAGE_OEMCP_BASE + TB_STAGE_OEMCP_SIZE)
    {
        const std::vector<uint8_t> &t = tb_stage_oemcp();
        uint32_t i = addr - TB_STAGE_OEMCP_BASE;
        return i < t.size() ? t[i] : 0;
    }
    std::map<uint32_t, uint8_t> &m = tb_stage_store();
    std::map<uint32_t, uint8_t>::const_iterator it = m.find(addr);
    return it == m.end() ? 0 : it->second;
}

#endif /* _TESTS_FPGA_TB_STAGE_H_ */
