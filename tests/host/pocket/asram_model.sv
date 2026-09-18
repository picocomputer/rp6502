/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * This module models the Pocket's SRAM, an AS6C2016-55BIN, which is a
 * 128K x 16 asynchronous part with a 55 ns access time. Analogue's port
 * list has no chip enable, which means CE# is tied low on the board and
 * the byte enables are the only deselect.
 *
 * The model has no delays, so a test that passes against it shows nothing
 * about the 55 ns access time.
 */

module asram_model (
    input logic [16:0] a,
    inout wire [15:0] dq,
    input logic oe_n,
    input logic we_n,
    input logic ub_n,
    input logic lb_n
);

    logic [15:0] mem[1 << 17] /*verilator public_flat_rw*/;

    logic drive_lo, drive_hi;
    always_comb begin
        drive_lo = !oe_n && we_n && !lb_n;
        drive_hi = !oe_n && we_n && !ub_n;
    end
    assign dq[7:0] = drive_lo ? mem[a][7:0] : 8'bz;
    assign dq[15:8] = drive_hi ? mem[a][15:8] : 8'bz;

    always_latch begin
        if (!we_n && !lb_n)
            mem[a][7:0] = dq[7:0];
        if (!we_n && !ub_n)
            mem[a][15:8] = dq[15:8];
    end

endmodule
