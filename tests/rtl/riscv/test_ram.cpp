/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * An AHB transfer's data phase overlaps the address phase of the transfer
 * after it, so a load issued right after a store to the same word reads the
 * cache's memories on the same clk_rv edge that the store writes them. A
 * read on that edge is not guaranteed to return the stored bytes, and in
 * the Verilator model it returns their complement. The load returns the
 * stored bytes because cache.sv forwards the written lanes, and soc.sv
 * gives it the address of the next access in every clock.
 */

#include "Vwiring.h"
#include "Vwiring___024root.h"

#include "tb_machine.h"
#include "utest.h"

#include <cstdint>

static Vwiring *dut;
static bool rv_phase;

static void run_program(const uint32_t *prog, unsigned words)
{
    dut->rst_n = 0;
    tb_clock(dut);
    tb_clock(dut);
    tb_ram_clear();
    for (unsigned i = 0; i < words; i++)
        tb_ram_put(i, prog[i]);
    dut->rst_n = 1;
    for (int i = 0; i < 2000; i++)
        tb_clock(dut);
    tb_ram_flush(dut, [] { tb_clock(dut); });
}

static uint32_t ram_peek(uint32_t byte_addr)
{
    return tb_ram_word(byte_addr >> 2);
}

UTEST(ram, load_after_store_sees_the_store)
{
    /* lui a5,0x1; li a4,0x12345678; sw a4,0(a5); lw a3,0(a5);
     * sw a3,4(a5); spin. */
    static const uint32_t prog[] = {
        0x000017b7, 0x12345737, 0x67870713, 0x00e7a023,
        0x0007a683, 0x00d7a223, 0x0000006f,
    };
    run_program(prog, sizeof prog / sizeof *prog);

    ASSERT_EQ(ram_peek(0x1000), (uint32_t)0x12345678);
    ASSERT_EQ(ram_peek(0x1004), (uint32_t)0x12345678);
}

/* The first store fetches the line, so the second finds it in the cache
 * and is answered with no wait, as the load after it is. */
UTEST(ram, load_after_a_store_that_hits)
{
    static const uint32_t prog[] = {
        0x000017b7,             /* lui  a5,0x1        */
        0x12345737,             /* lui  a4,0x12345    */
        0x67870713,             /* addi a4,a4,0x678   */
        0x00e7a023,             /* sw   a4,0(a5)      */
        0x00170713,             /* addi a4,a4,1       */
        0x00e7a023,             /* sw   a4,0(a5)      */
        0x0007a683,             /* lw   a3,0(a5)      */
        0x00d7a223,             /* sw   a3,4(a5)      */
        0x0000006f,             /* spin               */
    };
    run_program(prog, sizeof prog / sizeof *prog);

    ASSERT_EQ(ram_peek(0x1000), (uint32_t)0x12345679);
    ASSERT_EQ(ram_peek(0x1004), (uint32_t)0x12345679);
}

UTEST(ram, byte_store_then_byte_load)
{
    static const uint32_t prog[] = {
        0x000017b7,             /* lui  a5,0x1        */
        0x0a900713,             /* li   a4,0xa9       */
        0x00e78023,             /* sb   a4,0(a5)      */
        0x0007c683,             /* lbu  a3,0(a5)      */
        0x00d7a223,             /* sw   a3,4(a5)      */
        0x0000006f,             /* spin               */
    };
    run_program(prog, sizeof prog / sizeof *prog);

    ASSERT_EQ(ram_peek(0x1000) & 0xff, (uint32_t)0xa9);
    ASSERT_EQ(ram_peek(0x1004), (uint32_t)0xa9);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vwiring;
    dut->clk_sys = 0;
    dut->clk_rv = 0;
    dut->rst_n = 0;
    dut->eval();
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
