/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * This module is a behavioral model of the Pocket's SDRAM, an
 * AS4C32M16MSA-6BIN, which is a 512 Mbit x16 SDR part. The timing limits
 * are that datasheet's figures at 50.4 MHz, where a clock is 19.841 ns:
 *
 *   tRCD  18 ns    1 clk      tRAS min  48 ns     3 clk
 *   tRP   18 ns    1 clk      tRAS max 100 us  5040 clk
 *   tRC   60 ns    4 clk      tRFC      80 ns     5 clk
 *   tDPL   2 tCK   2 clk      tXSR      80 ns     5 clk
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
    output logic [31:0] sdram_model_refreshes,
    output logic [31:0] sdram_model_sref_clocks
);

    logic [15:0] mem[1 << 25] /*verilator public_flat_rw*/;

    logic [12:0] row[4];
    logic row_open[4];

    logic saw_pall;
    logic [1:0] saw_ref;
    logic saw_mrs;
    logic saw_emrs;

    /* PASR, Partial Array Self Refresh, sets how much of the array is
     * refreshed during self refresh, while an auto refresh covers the
     * whole array whatever PASR holds. The extended mode register has no
     * power-up default, so pasr is reset to the smallest setting the
     * encoding allows, and a self refresh entered before the extended
     * mode register is written marks banks 1 to 3 as lost. */
    logic [2:0] pasr;
    logic bank_lost[4];

    int since_act[4];
    int since_pre[4];
    int since_wr[4];
    int since_ref;
    int since_any;
    int since_srex;
    int since_sren;
    logic in_sref;

    logic [2:0] cmd;
    always_comb cmd = {ras_n, cas_n, we_n};

    /* Read data is valid at the third edge after the READ is sampled,
     * one edge later than CL2 alone, and that is the edge pocket_sdram
     * captures on. It is valid at that edge only, since the burst length
     * is one.
     *
     * dq_float changes on every clock rather than being 'x, because
     * Verilator is two-state and turns 'x into a constant that a capture
     * on the wrong edge would match whenever the true data equalled it. */
    logic [15:0] rd_p0, rd_p1, rd_p2;
    logic rd_v0, rd_v1, rd_v2;
    logic [15:0] dq_float;
    always_comb dq_out = rd_v2 ? rd_p2 : dq_float;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            for (int b = 0; b < 4; b++) begin
                row[b] <= '0;
                row_open[b] <= 1'b0;
                since_act[b] <= 100;
                since_pre[b] <= 100;
                since_wr[b] <= 100;
            end
            saw_pall <= 1'b0;
            saw_ref <= '0;
            saw_mrs <= 1'b0;
            saw_emrs <= 1'b0;
            pasr <= 3'b110; /* one sixteenth: the least it can be */
            for (int b = 0; b < 4; b++)
                bank_lost[b] <= 1'b0;
            since_ref <= 100;
            since_any <= 100;
            since_srex <= 100;
            since_sren <= 100;
            in_sref <= 1'b0;
            rd_p0 <= '0;
            rd_p1 <= '0;
            rd_p2 <= '0;
            rd_v0 <= 1'b0;
            rd_v1 <= 1'b0;
            rd_v2 <= 1'b0;
            dq_float <= 16'hFACE;
            sdram_model_refreshes <= '0;
            sdram_model_sref_clocks <= '0;
        end else begin
            rd_p1 <= rd_p0;
            rd_p2 <= rd_p1;
            rd_v0 <= 1'b0;
            rd_v1 <= rd_v0;
            rd_v2 <= rd_v1;
            dq_float <= dq_float + 16'h9E37;
            for (int b = 0; b < 4; b++) begin
                since_act[b] <= since_act[b] + 1;
                since_pre[b] <= since_pre[b] + 1;
                since_wr[b] <= since_wr[b] + 1;
                if (row_open[b] && since_act[b] > 5040)
                    $fatal(1, "sdram_model: tRAS max exceeded, bank %0d", b);
            end
            since_ref <= since_ref + 1;
            since_any <= since_any + 1;
            since_srex <= since_srex + 1;
            since_sren <= since_sren + 1;

            if (in_sref) begin
                sdram_model_sref_clocks <= sdram_model_sref_clocks + 32'd1;
                if (cke) begin
                    if (since_sren < 3)
                        $fatal(1, "sdram_model: self refresh shorter than tRAS");
                    in_sref <= 1'b0;
                    since_srex <= 0;
                    since_ref <= 0;
                end
            end else if (!cke) begin
                /* An AUTO REFRESH issued with CKE low is a self refresh
                 * entry. */
                if (cmd != 3'b001)
                    $fatal(1, "sdram_model: CKE low without self refresh entry");
                for (int b = 0; b < 4; b++)
                    if (row_open[b])
                        $fatal(1, "sdram_model: self refresh with an open row");
                for (int b = 0; b < 4; b++)
                    if (!(pasr == 3'b000                       /* all */
                          || (pasr == 3'b001 && b[1] == 1'b0)  /* half */
                          || b == 0))                          /* quarter or less */
                        bank_lost[b] <= 1'b1;
                in_sref <= 1'b1;
                since_sren <= 0;
            end else
            case (cmd)
                3'b111: ;  /* NOP */
                3'b010: begin  /* PRECHARGE */
                    if (a[10]) begin
                        for (int b = 0; b < 4; b++) begin
                            if (row_open[b] && since_act[b] < 3)
                                $fatal(1, "sdram_model: tRAS violated, bank %0d", b);
                            if (since_wr[b] < 2)
                                $fatal(1, "sdram_model: tWR violated, bank %0d", b);
                            row_open[b] <= 1'b0;
                            since_pre[b] <= 0;
                        end
                        saw_pall <= 1'b1;
                    end else begin
                        if (row_open[ba] && since_act[ba] < 3)
                            $fatal(1, "sdram_model: tRAS violated");
                        if (since_wr[ba] < 2)
                            $fatal(1, "sdram_model: tWR violated");
                        row_open[ba] <= 1'b0;
                        since_pre[ba] <= 0;
                    end
                    since_any <= 0;
                end
                3'b001: begin  /* AUTO REFRESH */
                    if (!saw_pall)
                        $fatal(1, "sdram_model: refresh before precharge");
                    for (int b = 0; b < 4; b++)
                        if (row_open[b])
                            $fatal(1, "sdram_model: refresh with open row");
                    if (since_ref < 5)
                        $fatal(1, "sdram_model: tRFC violated");
                    since_ref <= 0;
                    if (saw_ref != 2'd3)
                        saw_ref <= saw_ref + 2'd1;
                    sdram_model_refreshes <= sdram_model_refreshes + 32'd1;
                end
                3'b000:
                if (ba == 2'b10) begin
                    if (a[11:8] != 4'b0000)
                        $fatal(1, "sdram_model: EMRS reserved bits set, %b", a);
                    if (a[7:5] == 3'b101 || a[7:5] == 3'b110 || a[7:5] == 3'b111)
                        $fatal(1, "sdram_model: EMRS reserved driver strength");
                    if (a[2:0] == 3'b011 || a[2:0] == 3'b100 || a[2:0] == 3'b111)
                        $fatal(1, "sdram_model: EMRS reserved PASR");
                    pasr <= a[2:0];
                    saw_emrs <= 1'b1;
                end else begin
                    if (saw_ref < 2'd2)
                        $fatal(1, "sdram_model: MRS before two refreshes");
                    if (a != 13'b000_0_00_010_0_000)
                        $fatal(1, "sdram_model: unexpected mode %b", a);
                    saw_mrs <= 1'b1;
                end
                3'b011: begin  /* ACTIVE */
                    if (!saw_mrs)
                        $fatal(1, "sdram_model: ACT before init");
                    if (!saw_emrs)
                        $fatal(1, "sdram_model: ACT before EMRS");
                    if (bank_lost[ba])
                        $fatal(1, "sdram_model: bank %0d went unrefreshed while asleep, PASR %b", ba, pasr);
                    if (row_open[ba])
                        $fatal(1, "sdram_model: ACT on open bank");
                    if (since_pre[ba] < 1)
                        $fatal(1, "sdram_model: tRP violated");
                    if (since_act[ba] < 4)
                        $fatal(1, "sdram_model: tRC violated");
                    if (since_ref < 5)
                        $fatal(1, "sdram_model: tRFC before ACT");
                    if (since_srex < 5)
                        $fatal(1, "sdram_model: tXSR violated");
                    row[ba] <= a;
                    row_open[ba] <= 1'b1;
                    since_act[ba] <= 0;
                end
                3'b101: begin  /* READ */
                    if (!row_open[ba])
                        $fatal(1, "sdram_model: READ on closed bank");
                    if (since_act[ba] < 1)
                        $fatal(1, "sdram_model: tRCD violated on read");
                    rd_p0 <= mem[{ba, row[ba], a[9:0]}];
                    rd_v0 <= 1'b1;
                    if (a[10]) begin
                        row_open[ba] <= 1'b0;
                        since_pre[ba] <= 0;
                    end
                end
                3'b100: begin  /* WRITE */
                    if (!row_open[ba])
                        $fatal(1, "sdram_model: WRITE on closed bank");
                    if (since_act[ba] < 1)
                        $fatal(1, "sdram_model: tRCD violated on write");
                    if (!dq_oe)
                        $fatal(1, "sdram_model: write with dq released");
                    mem[{ba, row[ba], a[9:0]}] <= dq_in;
                    since_wr[ba] <= 0;
                    if (a[10]) begin
                        row_open[ba] <= 1'b0;
                        since_pre[ba] <= 0;
                    end
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
