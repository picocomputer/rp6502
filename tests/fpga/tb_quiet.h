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
 * Quiet means the 6502 is stopped or halted, a whole frame has passed
 * with no console byte moving, and the firmware has got as far as
 * running a program or refusing one. That last clause matters: a
 * machine that has not started looks exactly like one that has
 * finished, and waiting for the firmware to speak is not enough — it
 * announces the load before performing it, so a load longer than a
 * frame reads as a finished run and the test captures the boot
 * console. The firmware clears the staging length when it is done with
 * the image, accepted or not, which is the event to wait for; a test
 * that stages nothing has only the 6502 to go by. The caller supplies
 * the clock, because every test feeds the staging window and captures
 * console output its own way.
 *
 * The halt line is now an alarm rather than a finish line: only a
 * firmware that fell off the end of main can raise it, so a run that
 * does is a failed run.
 */

#ifndef _TESTS_FPGA_TB_QUIET_H_
#define _TESTS_FPGA_TB_QUIET_H_

template <typename Dut, typename Cycle>
static bool tb_quiet(Dut *dut, Cycle cycle, long frame_limit = 20)
{
    /* Bounded in clocks, not frames: a machine that never paints must
     * fail the run rather than spin the test forever. Twenty frames is
     * a third of a second of machine time and far more than any fixture
     * needs — the bound exists to fail fast, and a bound nobody can
     * afford to wait for is not a bound. */
    long budget = frame_limit * 1700000L;
    long frames = 0;
    bool moved = false;
    bool ran = false;
    /* An image waiting in the staging window; the firmware clears the
     * length when it is done with it, run or refused. */
    bool had_image = dut->rootp->rp6502__DOT__rv__DOT__mmio_slot_len != 0;
    bool pending = had_image;
    uint16_t prev = dut->rp6502_scanline;
    while (frames < frame_limit && budget-- > 0)
    {
        cycle();
        if (dut->rp6502_rv_halted)
            return false;
        if (dut->rp6502_tx_valid || dut->rp6502_rv_tx_valid)
            moved = true;
        if (dut->rootp->rp6502__DOT__cpu_run)
            ran = true;
        if (pending && !dut->rootp->rp6502__DOT__rv__DOT__mmio_slot_len)
            pending = false;
        uint16_t sl = dut->rp6502_scanline;
        bool frame_edge = sl == 0 && prev != 0;
        prev = sl;
        if (!frame_edge)
            continue;
        frames++;
        bool stopped = dut->rootp->rp6502__DOT__cpu__DOT__stop_flag != 0
            || !dut->rootp->rp6502__DOT__cpu_run;
        if (!pending && (ran || had_image) && stopped && !moved)
            return true;
        moved = false;
    }
    return false;
}

#endif /* _TESTS_FPGA_TB_QUIET_H_ */
