/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TESTS_BENCH_TB_RAM_H_
#define _TESTS_BENCH_TB_RAM_H_

#include <cstdint>
#include <cstdio>
#include <cstring>

/* The soft CPU's memory as the machine sees it behind its line port:
 * 512 KB that answer a line of 16 bytes at a time, where the Pocket has
 * its SDRAM. The ROM block is the first TB_RAM_ROM bytes, which hold the
 * program and take no store, and the RAM block starts after it. The cache
 * in soc.sv holds the newest copy of the lines the soft CPU has stored
 * to, so a program hands a test its results through the staging store. */
#define TB_RAM_BYTES 0x80000u
#define TB_RAM_ROM 0x40000u

/* A line takes this many clocks, about what the Pocket's SDRAM takes. */
#define TB_RAM_WAIT 16

struct tb_ram_t
{
    uint8_t mem[TB_RAM_BYTES];
    int wait;
    bool done;
};

static tb_ram_t &tb_ram()
{
    static tb_ram_t ram;
    return ram;
}

static void tb_ram_clear()
{
    memset(tb_ram().mem, 0, TB_RAM_BYTES);
}

static void tb_ram_put(uint32_t w, uint32_t v)
{
    memcpy(tb_ram().mem + w * 4, &v, 4);
}

/* Loads an image of the ROM block, which is what firmware.bin is. */
static bool tb_ram_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    tb_ram_clear();
    size_t n = fread(tb_ram().mem, 1, TB_RAM_ROM, f);
    bool whole = n < TB_RAM_ROM || fgetc(f) == EOF;
    fclose(f);
    return whole;
}

/* Called before every clock. It answers as a registered platform does:
 * line_done rises TB_RAM_WAIT clocks after the request shows and falls
 * the clock after the request does. A machine in reset has no request. */
template <typename Dut> static void tb_ram_tick(Dut *dut)
{
    tb_ram_t &ram = tb_ram();
    if (!dut->rst_n || !dut->wiring_line_pend)
    {
        ram.done = false;
        ram.wait = 0;
    }
    else if (!ram.done && ++ram.wait >= TB_RAM_WAIT)
    {
        uint8_t *line = ram.mem + (uint32_t)dut->wiring_line_addr * 16;
        if (dut->wiring_line_we)
            memcpy(line, dut->wiring_line_wdata.data(), 16);
        else
            memcpy(dut->line_rdata.data(), line, 16);
        ram.done = true;
    }
    dut->line_done = ram.done;
}

#endif /* _TESTS_BENCH_TB_RAM_H_ */
