/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Savestates, which on this platform are also sleep: at sleep the host
 * asks for a blob and cuts the power, and on wake it hands the blob
 * back and the machine picks up where it was. Wake reconfigures the
 * part, so the blob is the only thing that crosses.
 *
 * The two directions are not symmetric, the same way the file bridge's
 * are not. A load is already solved — the host writes the blob as
 * ordinary bridge writes and pocket_bridge puts them in the staging
 * store, where the firmware reads them like any staged image — so
 * nothing here touches that path except to notice it happened. A save
 * has nowhere to come from: the store is write-only from the host's
 * side and the machine cannot write it at all. So the firmware pushes
 * the blob a word at a time and this answers the host's reads out of a
 * small queue as they arrive.
 *
 * Analogue puts the host's floor at one bridge access per 88 cycles of
 * clk_74a and the firmware has about 29 clk_rv to make a word in, which
 * it stays ahead of by roughly half again on the slowest run of the
 * blob. The queue is for jitter, not for backlog. A read that finds it
 * empty anyway sets a sticky flag, and so does a push that finds it
 * full: either way the blob is wrong, and a save known to be wrong is
 * refused rather than handed over.
 *
 * The host re-reads an address it has already read, so the last word
 * served is held and given again rather than taken from the queue.
 * Only a new address advances it. Nothing here assumes the host reads
 * in order, but a blob served out of order would be wrong, and the
 * header check on the way back in is what catches that.
 *
 * Byte order is the store's, which is the byte stream's: byte zero of a
 * word rides bits 31:24, the same packing msc_win_put uses and the same
 * one pocket_bridge's halfword split undoes. A blob read out and
 * written back arrives byte for byte.
 */

module pocket_sst #(
    /* Where the host finds the blob. mmio.h carries the copy the
     * firmware reads it back through, and data.json gives up the room
     * at the top of the ROM slot; stage_map_gate.py checks all three.
     * The base is a whole megabyte, which is what lets the window
     * decode be a compare on the low twenty bits. */
    parameter logic [31:0] BLOB_BASE = 32'h03F0_0000,
    parameter logic [31:0] BLOB_WINDOW = 32'h000A_0000,
    /* Thirty-two words is 38 us at the host's floor, against a producer
     * that is never behind for more than one stalled bus access. */
    parameter int FIFO_LOG2 = 5
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
    /* Whether this address is ours to answer. Exported so the read mux
     * above is as exact as the decode here, rather than giving the
     * whole megabyte to the blob. */
    output logic pocket_sst_rd_sel,

    /* A level the core edge-detects, held until acknowledged. The
     * bridge's command engine parks in the command's own state until
     * the ack, so this is answered from the fabric and never waits on
     * the firmware. */
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

    /* What the firmware writes to SST_RESULT when it is finished. */
    localparam logic [2:0] RES_NONE = 3'd0;
    localparam logic [2:0] RES_START_OK = 3'd1;
    localparam logic [2:0] RES_START_ERR = 3'd2;
    localparam logic [2:0] RES_LOAD_OK = 3'd3;
    localparam logic [2:0] RES_LOAD_ERR = 3'd4;

    localparam logic [1:0] REG_CTL = 2'd0;
    localparam logic [1:0] REG_RESULT = 2'd1;
    localparam logic [1:0] REG_DATA = 2'd2;

    logic fifo_full, fifo_empty, fifo_take, data_we;
    logic [31:0] fifo_rdata;

    always_comb data_we = stb && we && addr[3:2] == REG_DATA;

    pocket_fifo #(
        .WIDTH(32),
        .DEPTH_LOG2(FIFO_LOG2)
    ) q (
        .wclk(clk_sys),
        .w_stb(data_we && !fifo_full),
        .w_data(wdata),
        .pocket_fifo_full(fifo_full),
        .rclk(clk_74a),
        .r_take(fifo_take),
        .pocket_fifo_empty(fifo_empty),
        .pocket_fifo_rdata(fifo_rdata)
    );

    /* --- The host's side. --- */

    logic blob_hit, in_window, rd_edge, dup;
    logic [17:0] rd_idx;
    always_comb begin
        in_window = bridge_addr[31:20] == BLOB_BASE[31:20]
            && bridge_addr[19:0] < BLOB_WINDOW[19:0];
        blob_hit = bridge_wr && in_window;
        rd_idx = bridge_addr[19:2];
    end

    logic bridge_rd_q, have_last, draining, blob_seen, underrun;
    logic [17:0] last_idx;
    logic [31:0] hold;
    logic start_q, load_q, start_pend, load_pend;
    logic [2:0] result;
    logic start_t, load_t;
    (* preserve *) logic res_t1, res_t2, res_t3;
    logic res_t;
    logic [2:0] res_code;

    always_comb begin
        rd_edge = bridge_rd && !bridge_rd_q && in_window;
        dup = have_last && rd_idx == last_idx;
        /* Show-ahead, so the word is latched and the pointer advanced
         * on the same edge. */
        fifo_take = draining ? !fifo_empty : (rd_edge && !dup && !fifo_empty);
    end

    always_comb begin
        pocket_sst_rd_data = hold;
        pocket_sst_rd_sel = in_window;
    end

    /* Answered from the level itself. The bridge only needs a cycle of
     * it, and anything slower is a cycle the command engine spends
     * parked for no reason. */
    always_comb begin
        pocket_sst_start_ack = savestate_start;
        pocket_sst_load_ack = savestate_load;
        pocket_sst_start_busy = start_pend;
        pocket_sst_load_busy = load_pend;
        pocket_sst_start_ok = result == RES_START_OK;
        pocket_sst_start_err = result == RES_START_ERR;
        pocket_sst_load_ok = result == RES_LOAD_OK;
        pocket_sst_load_err = result == RES_LOAD_ERR;
    end

    always_ff @(posedge clk_74a or negedge arst_n) begin
        if (!arst_n) begin
            bridge_rd_q <= 1'b0;
            have_last <= 1'b0;
            draining <= 1'b0;
            blob_seen <= 1'b0;
            underrun <= 1'b0;
            last_idx <= '0;
            hold <= '0;
            start_q <= 1'b0;
            load_q <= 1'b0;
            start_pend <= 1'b0;
            load_pend <= 1'b0;
            result <= RES_NONE;
            start_t <= 1'b0;
            load_t <= 1'b0;
            res_t1 <= 1'b0;
            res_t2 <= 1'b0;
            res_t3 <= 1'b0;
        end else begin
            bridge_rd_q <= bridge_rd;
            start_q <= savestate_start;
            load_q <= savestate_load;

            /* Sticky for the life of the core: at boot it is the whole
             * question of whether this is a wake or an ordinary load,
             * and the firmware asks it once before it starts the ROM. */
            if (blob_hit) blob_seen <= 1'b1;

            if (savestate_start && !start_q) begin
                start_pend <= 1'b1;
                result <= RES_NONE;
                underrun <= 1'b0;
                /* Whatever an abandoned save left behind is not part of
                 * this one. The firmware has not been told yet, so
                 * nothing is filling it and the drain terminates. */
                draining <= 1'b1;
                have_last <= 1'b0;
                start_t <= !start_t;
            end
            if (savestate_load && !load_q) begin
                load_pend <= 1'b1;
                result <= RES_NONE;
                load_t <= !load_t;
            end

            if (draining) begin
                if (fifo_empty) draining <= 1'b0;
            end else if (rd_edge && !dup) begin
                last_idx <= rd_idx;
                have_last <= 1'b1;
                if (fifo_empty) underrun <= 1'b1;
                else hold <= fifo_rdata;
            end

            res_t1 <= res_t;
            res_t2 <= res_t1;
            res_t3 <= res_t2;
            if (res_t2 != res_t3) begin
                result <= res_code;
                if (res_code == RES_START_OK || res_code == RES_START_ERR)
                    start_pend <= 1'b0;
                if (res_code == RES_LOAD_OK || res_code == RES_LOAD_ERR)
                    load_pend <= 1'b0;
            end
        end
    end

    /* --- The machine's side. --- */

    logic start_req, load_req, overrun;
    (* preserve *) logic start_s1, start_s2, start_s3;
    (* preserve *) logic load_s1, load_s2, load_s3;
    (* preserve *) logic seen_s1, seen_s2;
    (* preserve *) logic under_s1, under_s2;
    (* preserve *) logic empty_s1, empty_s2;

    initial begin
        pocket_sst_rdata = '0;
        start_req = 1'b0;
        load_req = 1'b0;
        overrun = 1'b0;
        res_code = RES_NONE;
        res_t = 1'b0;
        start_s1 = 1'b0;
        start_s2 = 1'b0;
        start_s3 = 1'b0;
        load_s1 = 1'b0;
        load_s2 = 1'b0;
        load_s3 = 1'b0;
        seen_s1 = 1'b0;
        seen_s2 = 1'b0;
        under_s1 = 1'b0;
        under_s2 = 1'b0;
        empty_s1 = 1'b1;
        empty_s2 = 1'b1;
    end

    always_ff @(posedge clk_sys) begin
        start_s1 <= start_t;
        start_s2 <= start_s1;
        start_s3 <= start_s2;
        load_s1 <= load_t;
        load_s2 <= load_s1;
        load_s3 <= load_s2;
        seen_s1 <= blob_seen;
        seen_s2 <= seen_s1;
        under_s1 <= underrun;
        under_s2 <= under_s1;
        empty_s1 <= fifo_empty;
        empty_s2 <= empty_s1;

        if (data_we && fifo_full) overrun <= 1'b1;

        if (stb && we)
            case (addr[3:2])
                REG_CTL: begin
                    if (wdata[0]) start_req <= 1'b0;
                    if (wdata[1]) load_req <= 1'b0;
                    if (wdata[4]) overrun <= 1'b0;
                end
                REG_RESULT: begin
                    res_code <= wdata[2:0];
                    res_t <= !res_t;
                end
                default: ;
            endcase

        /* After the clear, so a request that lands in the same cycle
         * the firmware acknowledges the last one is not the one that
         * gets lost. */
        if (start_s2 != start_s3) start_req <= 1'b1;
        if (load_s2 != load_s3) load_req <= 1'b1;

        if (stb)
            pocket_sst_rdata <= addr[3:2] == REG_CTL
                ? {25'd0, empty_s2, fifo_full, overrun, under_s2, seen_s2,
                   load_req, start_req}
                : 32'd0;
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_pocket_sst;
    always_comb unused_pocket_sst = we ^ (^addr[27:4]) ^ (^addr[1:0])
        ^ (^wdata[31:5]) ^ (^bridge_addr[31:20]);
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
