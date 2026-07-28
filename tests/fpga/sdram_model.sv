/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * A behavioral 512 Mbit x16 SDR SDRAM for the bench: commands sampled
 * on the clock, per-bank open rows, CL2 read pipe, JEDEC wake order
 * enforced — precharge-all, two refreshes, mode register — and the
 * coarse timing floors a real chip cares about, failed loudly with
 * $fatal so a sloppy controller cannot pass by accident.
 */

module sdram_model (
    input logic clk,
    input logic rst_n,
    input logic cke,
    input logic [12:0] a,
    input logic [1:0] ba,
    input logic ras_n,
    input logic cas_n,
    input logic we_n,
    input logic [15:0] dq_in,
    input logic dq_oe,
    output logic [15:0] dq_out,
    output logic [31:0] sdram_model_refreshes
);

    logic [15:0] mem[1 << 25];

    logic [12:0] row[4];
    logic row_open[4];

    /* Wake order tracking. */
    logic saw_pall;
    logic [1:0] saw_ref;
    logic saw_mrs;

    /* Coarse timing floors, in clocks at 100.8 MHz. */
    int since_act[4];
    int since_ref;
    int since_any;

    logic [2:0] cmd;
    always_comb cmd = {ras_n, cas_n, we_n};

    /* CL2: the answer crosses two registers after the READ command. */
    logic [15:0] rd_p0, rd_p1;
    always_comb dq_out = rd_p1;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            for (int b = 0; b < 4; b++) begin
                row[b] <= '0;
                row_open[b] <= 1'b0;
                since_act[b] <= 100;
            end
            saw_pall <= 1'b0;
            saw_ref <= '0;
            saw_mrs <= 1'b0;
            since_ref <= 100;
            since_any <= 100;
            rd_p0 <= '0;
            rd_p1 <= '0;
            sdram_model_refreshes <= '0;
        end else begin
            rd_p1 <= rd_p0;
            for (int b = 0; b < 4; b++)
                since_act[b] <= since_act[b] + 1;
            since_ref <= since_ref + 1;
            since_any <= since_any + 1;

            if (!cke)
                $fatal(1, "sdram_model: cke dropped");

            case (cmd)
                3'b111: ;  /* NOP */
                3'b010: begin  /* PRECHARGE */
                    if (a[10]) begin
                        for (int b = 0; b < 4; b++)
                            row_open[b] <= 1'b0;
                        saw_pall <= 1'b1;
                    end else
                        row_open[ba] <= 1'b0;
                    since_any <= 0;
                end
                3'b001: begin  /* AUTO REFRESH */
                    if (!saw_pall)
                        $fatal(1, "sdram_model: refresh before precharge");
                    for (int b = 0; b < 4; b++)
                        if (row_open[b])
                            $fatal(1, "sdram_model: refresh with open row");
                    if (since_ref < 8)
                        $fatal(1, "sdram_model: tRFC violated");
                    since_ref <= 0;
                    if (saw_ref != 2'd3)
                        saw_ref <= saw_ref + 2'd1;
                    sdram_model_refreshes <= sdram_model_refreshes + 32'd1;
                end
                3'b000: begin  /* MODE REGISTER SET */
                    if (saw_ref < 2'd2)
                        $fatal(1, "sdram_model: MRS before two refreshes");
                    if (a != 13'b000_0_00_010_0_000)
                        $fatal(1, "sdram_model: unexpected mode %b", a);
                    saw_mrs <= 1'b1;
                end
                3'b011: begin  /* ACTIVE */
                    if (!saw_mrs)
                        $fatal(1, "sdram_model: ACT before init");
                    if (row_open[ba])
                        $fatal(1, "sdram_model: ACT on open bank");
                    if (since_ref < 8)
                        $fatal(1, "sdram_model: tRFC before ACT");
                    row[ba] <= a;
                    row_open[ba] <= 1'b1;
                    since_act[ba] <= 0;
                end
                3'b101: begin  /* READ, expects A10 auto-precharge */
                    if (!row_open[ba])
                        $fatal(1, "sdram_model: READ on closed bank");
                    if (since_act[ba] < 2)
                        $fatal(1, "sdram_model: tRCD violated on read");
                    if (!a[10])
                        $fatal(1, "sdram_model: read without auto-precharge");
                    rd_p0 <= mem[{ba, row[ba], a[9:0]}];
                    row_open[ba] <= 1'b0;
                end
                3'b100: begin  /* WRITE, expects A10 auto-precharge */
                    if (!row_open[ba])
                        $fatal(1, "sdram_model: WRITE on closed bank");
                    if (since_act[ba] < 2)
                        $fatal(1, "sdram_model: tRCD violated on write");
                    if (!a[10])
                        $fatal(1, "sdram_model: write without auto-precharge");
                    if (!dq_oe)
                        $fatal(1, "sdram_model: write with dq released");
                    mem[{ba, row[ba], a[9:0]}] <= dq_in;
                    row_open[ba] <= 1'b0;
                end
                default:
                    $fatal(1, "sdram_model: illegal command %b", cmd);
            endcase
        end
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_sdram_model;
    always_comb unused_sdram_model = ^{a[12:11], 1'(since_any)};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
