/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * When a run is over, decided here rather than by the machine. Real
 * firmware never finishes, so the soft CPU's OS loop runs forever the
 * way it will on hardware; the testbench is what wants to know that
 * the program has stopped and the console has gone still, and this is
 * where that judgment belongs.
 *
 * Quiet means the firmware has spoken at least once, the 6502 is
 * stopped or halted, and a whole frame has passed with no console
 * byte moving. The first clause matters: a machine fresh out of reset
 * has a stopped 6502 and a silent console, and is not finished — it
 * has not started. The caller supplies the clock, because every test
 * feeds the staging window and captures console output its own way.
 *
 * The halt line is now an alarm rather than a finish line: only a
 * firmware that fell off the end of main can raise it, so a run that
 * does is a failed run.
 */

#ifndef _TESTS_FPGA_TB_QUIET_H_
#define _TESTS_FPGA_TB_QUIET_H_

template <typename Dut, typename Cycle>
static bool tb_quiet(Dut *dut, Cycle cycle, long frame_limit = 600)
{
    /* Bounded in clocks, not frames: a machine that never paints must
     * fail the run rather than spin the test forever. */
    long budget = frame_limit * 1700000L;
    long frames = 0;
    bool moved = false;
    bool spoke = false;
    uint16_t prev = dut->rp6502_scanline;
    while (frames < frame_limit && budget-- > 0)
    {
        cycle();
        if (dut->rp6502_rv_halted)
            return false;
        if (dut->rp6502_tx_valid || dut->rp6502_rv_tx_valid)
            moved = spoke = true;
        uint16_t sl = dut->rp6502_scanline;
        bool frame_edge = sl == 0 && prev != 0;
        prev = sl;
        if (!frame_edge)
            continue;
        frames++;
        bool stopped = dut->rootp->rp6502__DOT__cpu__DOT__stop_flag != 0
            || !dut->rootp->rp6502__DOT__cpu_run;
        if (spoke && stopped && !moved)
            return true;
        moved = false;
    }
    return false;
}

#endif /* _TESTS_FPGA_TB_QUIET_H_ */
