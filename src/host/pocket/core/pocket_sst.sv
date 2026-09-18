/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The ack and status signals of the host's savestate commands are
 * driven here and not by the firmware, because sst_engine halts the
 * soft CPU at its debug port while a blob is made or loaded.
 */

module pocket_sst #(
    /* BLOB_BASE and BLOB_WINDOW are core_top's savestate_addr and
     * savestate_maxloadsize. BLOB_BASE is aligned to 1 MB so that the
     * window decode compares bits 31:20 with the base and bits 19:0
     * with BLOB_WINDOW. */
    parameter logic [31:0] BLOB_BASE = 32'h03F0_0000,
    parameter logic [31:0] BLOB_WINDOW = 32'h000A_0000,
    /* BLOB_WORDS is sst_engine's word count and core_top's
     * savestate_size divided by four, and stage_map_gate.py checks that
     * the three agree. No host command marks the end of reading the
     * blob, so the save request is dropped when the word at the last
     * index arrives from sst_engine. */
    parameter int BLOB_WORDS = 81236
) (
    input logic clk_sys,
    input logic stb,
    input logic we,
    input logic [27:0] addr,
    input logic [31:0] wdata,
    output logic [31:0] pocket_sst_rdata,

    input logic clk_74a,
    input logic arst_n,
    input logic bridge_wr,
    input logic bridge_rd,
    input logic [31:0] bridge_addr,
    output logic [31:0] pocket_sst_rd_data,
    output logic pocket_sst_rd_sel,

    output logic pocket_sst_save,
    input logic sst_ready,
    output logic [17:0] pocket_sst_rd_idx,
    output logic pocket_sst_rd_t,
    input logic [31:0] sst_rdata,
    input logic sst_rvalid,

    output logic pocket_sst_load,
    input logic sst_load_done,
    input logic sst_load_err,

    input logic savestate_start,
    output logic pocket_sst_start_ack,
    output logic pocket_sst_start_busy,
    output logic pocket_sst_start_ok,
    output logic pocket_sst_start_err,

    input logic savestate_load,
    output logic pocket_sst_load_ack,
    output logic pocket_sst_load_busy,
    output logic pocket_sst_load_ok,
    output logic pocket_sst_load_err
);

    localparam logic [17:0] LAST_IDX = 18'(BLOB_WORDS - 1);

    localparam logic [1:0] REG_CTL = 2'd0;

    logic in_window, rd_edge;
    logic [17:0] rd_idx;
    logic bridge_rd_q;
    logic [31:0] hold;

    logic [17:0] ask;
    logic ask_t, asked, hold_valid, seen_low, late;
    (* preserve *) logic rvalid_s1, rvalid_s2;
    (* preserve *) logic ready_s1, ready_s2;
    (* preserve *) logic ldone_s1, ldone_s2;
    (* preserve *) logic lerr_s1, lerr_s2;

    logic blob_hit;
    always_comb begin
        in_window = bridge_addr[31:20] == BLOB_BASE[31:20]
            && bridge_addr[19:0] < BLOB_WINDOW[19:0];
        blob_hit = bridge_wr && in_window;
        rd_idx = bridge_addr[19:2];
        rd_edge = bridge_rd && !bridge_rd_q && in_window;
        pocket_sst_rd_data = hold;
        pocket_sst_rd_sel = in_window;
        pocket_sst_save = start_hold;
        pocket_sst_rd_idx = ask;
        pocket_sst_rd_t = ask_t;
    end

    logic blob_seen;
    logic start_q, load_q;
    logic start_hold, start_done;
    logic load_hold, load_kept, load_bad;
    logic load_t;

    always_comb begin
        pocket_sst_start_ack = savestate_start;
        pocket_sst_load_ack = savestate_load;
        pocket_sst_start_busy = start_hold && !start_done;
        pocket_sst_start_ok = start_done && !late;
        pocket_sst_start_err = start_done && late;
        pocket_sst_load_busy = load_hold && !load_kept;
        pocket_sst_load_ok = load_kept && !load_bad;
        pocket_sst_load_err = load_kept && load_bad;
    end

    always_ff @(posedge clk_74a or negedge arst_n) begin
        if (!arst_n) begin
            blob_seen <= 1'b0;
            bridge_rd_q <= 1'b0;
            hold <= '0;
            ask <= '0;
            ask_t <= 1'b0;
            asked <= 1'b0;
            hold_valid <= 1'b0;
            seen_low <= 1'b0;
            late <= 1'b0;
            rvalid_s1 <= 1'b0;
            rvalid_s2 <= 1'b0;
            ready_s1 <= 1'b0;
            ready_s2 <= 1'b0;
            ldone_s1 <= 1'b0;
            ldone_s2 <= 1'b0;
            lerr_s1 <= 1'b0;
            lerr_s2 <= 1'b0;
            start_q <= 1'b0;
            load_q <= 1'b0;
            start_hold <= 1'b0;
            start_done <= 1'b0;
            load_hold <= 1'b0;
            load_kept <= 1'b0;
            load_bad <= 1'b0;
            load_t <= 1'b0;
        end else begin
            start_q <= savestate_start;
            load_q <= savestate_load;
            bridge_rd_q <= bridge_rd;
            rvalid_s1 <= sst_rvalid;
            rvalid_s2 <= rvalid_s1;
            ready_s1 <= sst_ready;
            ready_s2 <= ready_s1;
            ldone_s1 <= sst_load_done;
            ldone_s2 <= ldone_s1;
            lerr_s1 <= sst_load_err;
            lerr_s2 <= lerr_s1;

            /* sst_engine keeps rvalid high for the previous request at
             * least until the new one has crossed into its clock domain,
             * so a word is taken only after rvalid_s2 has been low once
             * since the strobe. */
            if (!rvalid_s2) seen_low <= 1'b1;
            else if (seen_low && !hold_valid) begin
                hold <= sst_rdata;
                hold_valid <= 1'b1;
                /* The save request is dropped here, when the last word
                 * arrives, and not at the strobe for that word, because
                 * sst_engine checks the save request before a pending
                 * read and would resume the machine without serving
                 * it. */
                if (asked && ask == LAST_IDX) start_hold <= 1'b0;
            end

            if (rd_edge) begin
                /* io_bridge_peripheral.v sends the host the value on
                 * bridge_rd_data and only then strobes bridge_rd for the
                 * new address, so each read returns the word addressed
                 * by the read before it. A word that is still not in
                 * hold at the next strobe has therefore not been sent,
                 * and late is set so that the save's result is reported
                 * as an error. */
                if (asked && !hold_valid) late <= 1'b1;
                ask <= rd_idx;
                ask_t <= !ask_t;
                asked <= 1'b1;
                hold_valid <= 1'b0;
                seen_low <= 1'b0;
            end

            /* The host writes a blob into the window before it sends the
             * load command, so a write there is the first sign of a
             * restore. The firmware reads blob_seen as a restore in
             * progress, so it is cleared when the load finishes. */
            if (load_hold && ldone_s2 && !load_kept) blob_seen <= 1'b0;
            if (blob_hit) blob_seen <= 1'b1;

            if (start_hold && ready_s2) start_done <= 1'b1;
            if (load_hold && ldone_s2) begin
                load_kept <= 1'b1;
                load_bad <= lerr_s2;
            end

            if (savestate_start && !start_q) begin
                start_hold <= 1'b1;
                start_done <= 1'b0;
                asked <= 1'b0;
                hold_valid <= 1'b0;
                seen_low <= 1'b0;
                late <= 1'b0;
            end
            if (savestate_load && !load_q) begin
                load_hold <= 1'b1;
                load_kept <= 1'b0;
                load_bad <= 1'b0;
                load_t <= !load_t;
            end
        end
    end

    logic load_req;
    (* preserve *) logic load_s1, load_s2, load_s3;
    (* preserve *) logic seen_s1, seen_s2;
    (* preserve *) logic under_s1, under_s2;

    initial begin
        pocket_sst_rdata = '0;
        load_req = 1'b0;
        load_s1 = 1'b0;
        load_s2 = 1'b0;
        load_s3 = 1'b0;
        seen_s1 = 1'b0;
        seen_s2 = 1'b0;
        under_s1 = 1'b0;
        under_s2 = 1'b0;
    end

    always_comb pocket_sst_load = load_req;

    always_ff @(posedge clk_sys) begin
        load_s1 <= load_t;
        load_s2 <= load_s1;
        load_s3 <= load_s2;
        seen_s1 <= blob_seen;
        seen_s2 <= seen_s1;
        under_s1 <= late;
        under_s2 <= under_s1;

        if (stb && we && addr[3:2] == REG_CTL && wdata[0]) load_req <= 1'b0;

        /* This comes after the clear, so a load request that arrives in
         * the same cycle as the firmware's clear of the previous one is
         * kept. */
        if (load_s2 != load_s3) load_req <= 1'b1;

        /* A read of bit 0 returns sst_load_done and not load_req, so the
         * firmware reads it as set only after sst_engine has finished the
         * load and released the soft CPU. After a successful load the 6502
         * stays in reset until the firmware writes 1 to bit 0. */
        if (stb)
            pocket_sst_rdata <= addr[3:2] == REG_CTL
                ? {28'd0, sst_load_err, under_s2, seen_s2, sst_load_done}
                : 32'd0;
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_pocket_sst;
    always_comb unused_pocket_sst = we ^ (^addr[27:4]) ^ (^addr[1:0])
        ^ (^wdata[31:1]) ^ (^bridge_addr[31:20]) ^ ready_s1;
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
