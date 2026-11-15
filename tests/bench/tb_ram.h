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

/* The soft CPU's RAM as the machine sees it behind its line port: 256 KB
 * that answer a line of 16 bytes at a time, where the Pocket has its
 * SDRAM. The cache in soc.sv holds the newest copy of the lines the soft
 * CPU has stored to, so a test reads this array after tb_ram_flush. */
#define TB_RAM_BYTES 0x40000u

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

static uint32_t tb_ram_word(uint32_t w)
{
    uint32_t v;
    memcpy(&v, tb_ram().mem + w * 4, 4);
    return v;
}

static void tb_ram_put(uint32_t w, uint32_t v)
{
    memcpy(tb_ram().mem + w * 4, &v, 4);
}

static bool tb_ram_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    tb_ram_clear();
    size_t n = fread(tb_ram().mem, 1, TB_RAM_BYTES, f);
    bool whole = n < TB_RAM_BYTES || fgetc(f) == EOF;
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

/* Has the cache write out every line it holds a newer copy of. The soft
 * CPU waits while it does, and goes on after. */
template <typename Dut, typename Clock>
static void tb_ram_flush(Dut *dut, Clock clock)
{
    dut->sst_flush = 1;
    while (dut->wiring_sst_cache_idle)
        clock();
    while (!dut->wiring_sst_cache_idle)
        clock();
    dut->sst_flush = 0;
}

#endif /* _TESTS_BENCH_TB_RAM_H_ */
