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
 * This is the command half. The bridge's command engine parks in the
 * savestate state until the ack, so the ack is answered here in the
 * fabric and never waits on the firmware; the request is latched for
 * the machine to poll, and the machine answers with ok or error when it
 * is finished.
 *
 * A bridge write into the blob window raises a sticky bit, because that
 * is how a wake announces itself before any command arrives: the host
 * writes the blob in as ordinary bridge writes, which pocket_bridge
 * puts in the staging store, and only then asks for the load. A
 * firmware that finds the bit set at boot knows not to start the ROM.
 *
 * Serving the blob out is not here, and it is not a stream. The blob is
 * the machine's four memories; the thing that reads them is the
 * machine's own bus, with the machine frozen and this side holding it.
 * It was a queue fed by the firmware once, which measured at 3124 ns a
 * word against the host's 1185 ns floor -- 2.6 times short, and short
 * every word rather than in bursts, which is the one thing a queue
 * cannot absorb.
 */

module pocket_sst #(
    /* Where the host finds the blob. mmio.h carries the copy the
     * firmware reads it back through, and data.json gives up the room
     * at the top of the ROM slot; stage_map_gate.py checks all three.
     * The base is a whole megabyte, which is what lets the window
     * decode be a compare on the low twenty bits. */
    parameter logic [31:0] BLOB_BASE = 32'h03F0_0000,
    parameter logic [31:0] BLOB_WINDOW = 32'h000A_0000
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
    input logic [31:0] bridge_addr,

    /* A level the core edge-detects, held until acknowledged. */
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

    /* --- The host's side. --- */

    logic blob_hit;
    always_comb
        blob_hit = bridge_wr && bridge_addr[31:20] == BLOB_BASE[31:20]
            && bridge_addr[19:0] < BLOB_WINDOW[19:0];

    logic blob_seen;
    logic start_q, load_q, start_pend, load_pend;
    logic [2:0] result;
    logic start_t, load_t;
    (* preserve *) logic res_t1, res_t2, res_t3;
    logic res_t;
    logic [2:0] res_code;

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
            blob_seen <= 1'b0;
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
            start_q <= savestate_start;
            load_q <= savestate_load;

            /* Sticky for the life of the core: at boot it is the whole
             * question of whether this is a wake or an ordinary load,
             * and the firmware asks it once before it starts the ROM. */
            if (blob_hit) blob_seen <= 1'b1;

            if (savestate_start && !start_q) begin
                start_pend <= 1'b1;
                result <= RES_NONE;
                start_t <= !start_t;
            end
            if (savestate_load && !load_q) begin
                load_pend <= 1'b1;
                result <= RES_NONE;
                load_t <= !load_t;
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

    logic start_req, load_req;
    (* preserve *) logic start_s1, start_s2, start_s3;
    (* preserve *) logic load_s1, load_s2, load_s3;
    (* preserve *) logic seen_s1, seen_s2;

    initial begin
        pocket_sst_rdata = '0;
        start_req = 1'b0;
        load_req = 1'b0;
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

        if (stb && we)
            case (addr[3:2])
                REG_CTL: begin
                    if (wdata[0]) start_req <= 1'b0;
                    if (wdata[1]) load_req <= 1'b0;
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
                ? {29'd0, seen_s2, load_req, start_req}
                : 32'd0;
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_pocket_sst;
    always_comb unused_pocket_sst = we ^ (^addr[27:4]) ^ (^addr[1:0])
        ^ (^wdata[31:3]) ^ (^bridge_addr[31:20]);
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
