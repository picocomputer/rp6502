/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TESTS_FPGA_TB_QUIET_H_
#define _TESTS_FPGA_TB_QUIET_H_

template <typename Dut, typename Cycle>
static bool tb_quiet(Dut *dut, Cycle cycle, long frame_limit = 20)
{
    long budget = frame_limit * 1700000L;
    long frames = 0;
    bool moved = false;
    bool ran = false;
    int quiet_frames = 0;
    /* The 6502 is held in reset while the firmware loads a staged image,
     * so a load that prints nothing across two frame edges would
     * otherwise pass for a finished run. main_stage clears the staging
     * length only after the load succeeds or fails. */
    bool had_image = dut->rootp->wiring__DOT__soc__DOT__mmio_slot_len != 0;
    bool pending = had_image;
    uint16_t prev = dut->wiring_scanline;
    while (frames < frame_limit && budget-- > 0)
    {
        cycle();
        if (dut->wiring_rv_halted)
            return false;
        if (dut->wiring_tx_valid || dut->wiring_rv_tx_valid)
            moved = true;
        if (dut->rootp->wiring__DOT__resb)
            ran = true;
        if (pending && !dut->rootp->wiring__DOT__soc__DOT__mmio_slot_len)
            pending = false;
        uint16_t sl = dut->wiring_scanline;
        bool frame_edge = sl == 0 && prev != 0;
        prev = sl;
        if (!frame_edge)
            continue;
        frames++;
        bool stopped = dut->rootp->wiring__DOT__cpu__DOT__stop_flag != 0
            || !dut->rootp->wiring__DOT__resb;
        /* Two quiet frames are required because main_stage clears the
         * staging length before sys_commit releases the 6502. At a frame
         * edge between the two, the load has finished and the 6502 is
         * still in reset, which is also the state a rejected image leaves. */
        if (!pending && (ran || had_image) && stopped && !moved)
        {
            if (++quiet_frames >= 2)
                return true;
        }
        else
            quiet_frames = 0;
        moved = false;
    }
    return false;
}

#endif /* _TESTS_FPGA_TB_QUIET_H_ */
