/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The host's savestate commands, answered for the firmware, which makes
 * and loads the savestate in the blob window of the SDRAM.
 *
 * core_bridge_cmd holds a request line high until it sees the ack, and
 * the result it gives the host is the status lines in the cycle of the
 * ack. The ack is therefore one clock after the request's rising edge,
 * when the status registers already describe the new request.
 *
 * A save is passed to the firmware in SAVE_REQ and answers busy until
 * the firmware writes SAVE_OK or SAVE_ERR, then ok or err. The host then
 * reads the window, and each of its reads is served from the SDRAM. A
 * word not served in time turns the save's ok into err.
 *
 * A load is passed to the firmware in LOAD_REQ. One asked for while the
 * machine runs answers busy until the firmware writes LOAD_OK or
 * LOAD_ERR. One asked for while the host holds the core in reset
 * answers ok at once and keeps answering ok, because the host may wait
 * for the answer before it sends Reset Exit. Its request is kept for the
 * firmware that boots after the reset.
 *
 * A request that arrives while one of its kind is with the firmware is
 * acknowledged, answered with that one's status, and starts nothing.
 *
 * The firmware's registers, by addr[3:2]:
 *
 *   0  SST_CTL. Reads SAVE_REQ in bit 0 and LOAD_REQ in bit 1, each
 *      set from the host's request until the firmware answers it.
 *      BLOB_SEEN, bit 2, is set by a host write into the window or a
 *      load request, either of which can come first, and clears when
 *      the load is answered. WQ_EMPTY, bit 3, is set while the bridge's
 *      queue of writes to the SDRAM is empty, so the host's blob is all
 *      in the SDRAM. A write of 1 is SAVE_OK, 2 SAVE_ERR, 3 LOAD_OK and
 *      4 LOAD_ERR.
 *
 * Where the Pocket's side of this comes from:
 * - The two commands and their busy, ok and err results, and that the
 *   host reads the window once a save is ok: Analogue's docs,
 *   https://www.analogue.co/developer/docs/openfpga/host-target-commands
 * - The request, ack and status lines: Analogue's template,
 *   core_bridge_cmd.v in vendor/openfpga,
 *   https://github.com/open-fpga/core-template
 * - Busy while the firmware works: the template's notes, and the
 *   Spacewar core, https://github.com/spacemen3/PDP-1, Analogue's own
 *   example, which answers busy while it waits up to two video frames.
 * - A load in reset answers ok: Spacewar's savestate logic runs on its
 *   own power-on reset, not on the host's reset_n, so it answers a load
 *   in reset. A community PlayStation core,
 *   https://github.com/GuardianSoul/openfpga-PlayStation, reports that a load can
 *   arrive while the core is held in reset.
 * - A host write into the window is the blob arriving: Spacewar takes
 *   the host's writes straight into its RAM at savestate_addr.
 * - A load writes the savestate at savestate_addr with nothing in front:
 *   measured on a Pocket.
 * - The host serves no file command while a load is unanswered, so the
 *   firmware answers before it reopens any file: measured on a Pocket.
 * - Reads are buffered one word, so a word late at the next strobe is
 *   err: the template's io_bridge_peripheral.v.
 * - The order of a wake's blob, its 0x00A4 and Reset Exit: not known.
 *   A Pocket left a wake's load unanswered while the firmware ran, so
 *   that 0x00A4 came after Reset Exit (inferred). The core takes them in
 *   any order.
 */

module pocket_sst #(
    /* BLOB_BASE and BLOB_WINDOW are core_top's savestate_addr and
     * savestate_maxloadsize. The window is the last thing in the SDRAM
     * and runs to its end, 64 MB. */
    parameter logic [31:0] BLOB_BASE = 32'h03EC_1000,
    parameter logic [31:0] BLOB_WINDOW = 32'h0013_F000
) (
    input logic clk_sys,
    input logic stb,
    input logic we,
    input logic [27:0] addr,
    input logic [31:0] wdata,
    output logic [31:0] pocket_sst_rdata,
    input logic w_queued,

    /* The SDRAM's read client, as pocket_sdram.sv has it. The port is
     * this module's while pocket_sst_rd_pend is high. */
    output logic pocket_sst_rd_pend,
    output logic [23:0] pocket_sst_rd_addr,
    input logic rd_rvalid,
    input logic [31:0] rd_rword,

    input logic clk_74a,
    input logic arst_n,
    input logic bridge_wr,
    input logic bridge_rd,
    input logic [31:0] bridge_addr,
    /* core_bridge_cmd's reset_n, low until the host's Reset Exit. */
    input logic reset_n,
    output logic [31:0] pocket_sst_rd_data,
    output logic pocket_sst_rd_sel,

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

    localparam logic [2:0] CODE_SAVE_OK = 3'd1;
    localparam logic [2:0] CODE_SAVE_ERR = 3'd2;
    localparam logic [2:0] CODE_LOAD_OK = 3'd3;
    localparam logic [2:0] CODE_LOAD_ERR = 3'd4;

    /* A read is fetched from the SDRAM word at its own address. */
    localparam logic [31:0] BLOB_END = BLOB_BASE + BLOB_WINDOW;
    logic in_window, rd_edge, blob_hit;
    logic [23:0] rd_idx;
    always_comb begin
        in_window = bridge_addr >= BLOB_BASE && bridge_addr < BLOB_END;
        blob_hit = bridge_wr && in_window;
        rd_idx = bridge_addr[25:2];
        rd_edge = bridge_rd && !bridge_rd_q && in_window;
        pocket_sst_rd_data = hold;
        pocket_sst_rd_sel = in_window;
    end

    /* The clk_74a side. save_act and load_act are high from a request
     * until the firmware's answer is back. load_early marks a load asked
     * for in reset, whose answer was ok from the start. */
    logic bridge_rd_q, start_q, load_q;
    logic save_act, save_done, save_bad;
    logic load_act, load_early, load_ok, load_bad;
    logic save_t, load_t;
    logic blob_seen;
    logic [23:0] ask;
    logic ask_t, asked, hold_valid, late;
    logic [31:0] hold;
    (* preserve *) logic got_s1, got_s2;
    (* preserve *) logic sans_s1, sans_s2;
    (* preserve *) logic lans_s1, lans_s2;
    logic sans_q, lans_q;

    always_comb begin
        pocket_sst_start_ack = savestate_start && start_q;
        pocket_sst_load_ack = savestate_load && load_q;
        pocket_sst_start_busy = save_act;
        pocket_sst_start_ok = save_done && !save_bad && !late;
        pocket_sst_start_err = save_done && (save_bad || late);
        pocket_sst_load_busy = load_act && !load_ok;
        pocket_sst_load_ok = load_ok;
        pocket_sst_load_err = load_bad;
    end

    always_ff @(posedge clk_74a or negedge arst_n) begin
        if (!arst_n) begin
            bridge_rd_q <= 1'b0;
            start_q <= 1'b0;
            load_q <= 1'b0;
            save_act <= 1'b0;
            save_done <= 1'b0;
            save_bad <= 1'b0;
            load_act <= 1'b0;
            load_early <= 1'b0;
            load_ok <= 1'b0;
            load_bad <= 1'b0;
            save_t <= 1'b0;
            load_t <= 1'b0;
            blob_seen <= 1'b0;
            ask <= '0;
            ask_t <= 1'b0;
            asked <= 1'b0;
            hold_valid <= 1'b0;
            late <= 1'b0;
            hold <= '0;
            got_s1 <= 1'b0;
            got_s2 <= 1'b0;
            sans_s1 <= 1'b0;
            sans_s2 <= 1'b0;
            lans_s1 <= 1'b0;
            lans_s2 <= 1'b0;
            sans_q <= 1'b0;
            lans_q <= 1'b0;
        end else begin
            bridge_rd_q <= bridge_rd;
            start_q <= savestate_start;
            load_q <= savestate_load;
            got_s1 <= got_t;
            got_s2 <= got_s1;
            sans_s1 <= save_ans_t;
            sans_s2 <= sans_s1;
            sans_q <= sans_s2;
            lans_s1 <= load_ans_t;
            lans_s2 <= lans_s1;
            lans_q <= lans_s2;

            /* io_bridge_peripheral.v sends the host the value on
             * bridge_rd_data and only then strobes bridge_rd for the new
             * address, so each read returns the word addressed by the
             * read before it. A word that is still not in hold at the
             * next strobe has therefore not been sent, and late marks the
             * save as failed. */
            if (rd_edge) begin
                if (asked && !hold_valid) late <= 1'b1;
                ask <= rd_idx;
                ask_t <= !ask_t;
                asked <= 1'b1;
                hold_valid <= 1'b0;
            end else if (asked && !hold_valid && got_s2 == ask_t) begin
                hold <= word;
                hold_valid <= 1'b1;
            end

            if (sans_s2 != sans_q) begin
                save_act <= 1'b0;
                save_done <= 1'b1;
                save_bad <= save_err;
            end
            if (lans_s2 != lans_q) begin
                load_act <= 1'b0;
                blob_seen <= 1'b0;
                if (!load_early) begin
                    load_ok <= !load_err;
                    load_bad <= load_err;
                end
            end
            if (blob_hit) blob_seen <= 1'b1;

            if (savestate_start && !start_q && !save_act) begin
                save_act <= 1'b1;
                save_done <= 1'b0;
                save_bad <= 1'b0;
                save_t <= !save_t;
                late <= 1'b0;
                asked <= 1'b0;
                hold_valid <= 1'b0;
            end
            if (savestate_load && !load_q && !load_act) begin
                load_act <= 1'b1;
                load_early <= !reset_n;
                load_ok <= !reset_n;
                load_bad <= 1'b0;
                load_t <= !load_t;
                blob_seen <= 1'b1;
            end
        end
    end

    /* The clk_sys side. save_req and load_req have no reset: a load asked
     * for in reset is still here when the firmware boots. The answers go
     * back as toggles, with save_err and load_err set on the clock that
     * flips them and read only once the flip has crossed. */
    (* preserve *) logic save_s1, save_s2, load_s1, load_s2;
    (* preserve *) logic seen_s1, seen_s2;
    (* preserve *) logic ask_s1, ask_s2;
    logic save_sq, load_sq;
    logic save_req, load_req;
    logic save_ans_t, load_ans_t, save_err, load_err;
    logic fetch, fetch_t, got_t;
    logic [23:0] fetch_idx;
    logic [31:0] word;
    logic ctl_we;
    always_comb begin
        ctl_we = stb && we && addr[3:2] == 2'd0;
        pocket_sst_rd_pend = fetch;
        pocket_sst_rd_addr = fetch_idx;
    end

    initial begin
        pocket_sst_rdata = '0;
        save_s1 = 1'b0;
        save_s2 = 1'b0;
        save_sq = 1'b0;
        load_s1 = 1'b0;
        load_s2 = 1'b0;
        load_sq = 1'b0;
        seen_s1 = 1'b0;
        seen_s2 = 1'b0;
        ask_s1 = 1'b0;
        ask_s2 = 1'b0;
        save_req = 1'b0;
        load_req = 1'b0;
        save_ans_t = 1'b0;
        load_ans_t = 1'b0;
        save_err = 1'b0;
        load_err = 1'b0;
        fetch = 1'b0;
        fetch_t = 1'b0;
        got_t = 1'b0;
        fetch_idx = '0;
        word = '0;
    end
    always_ff @(posedge clk_sys) begin
        save_s1 <= save_t;
        save_s2 <= save_s1;
        save_sq <= save_s2;
        load_s1 <= load_t;
        load_s2 <= load_s1;
        load_sq <= load_s2;
        seen_s1 <= blob_seen;
        seen_s2 <= seen_s1;
        ask_s1 <= ask_t;
        ask_s2 <= ask_s1;

        if (ctl_we && save_req
            && (wdata[2:0] == CODE_SAVE_OK || wdata[2:0] == CODE_SAVE_ERR))
        begin
            save_req <= 1'b0;
            save_err <= wdata[2:0] == CODE_SAVE_ERR;
            save_ans_t <= !save_ans_t;
        end
        if (ctl_we && load_req
            && (wdata[2:0] == CODE_LOAD_OK || wdata[2:0] == CODE_LOAD_ERR))
        begin
            load_req <= 1'b0;
            load_err <= wdata[2:0] == CODE_LOAD_ERR;
            load_ans_t <= !load_ans_t;
        end
        if (save_s2 != save_sq) save_req <= 1'b1;
        if (load_s2 != load_sq) load_req <= 1'b1;

        /* ask has been still since ask_t flipped, so it is taken once the
         * flip has crossed. The SDRAM's words are a little-endian pair of
         * halfwords and the bridge's are big-endian, as pocket_bridge.sv
         * writes them, so the bytes go back in the order the window's
         * writer gave them. */
        if (!fetch && ask_s2 != got_t) begin
            fetch <= 1'b1;
            fetch_t <= ask_s2;
            fetch_idx <= ask;
        end
        if (fetch && rd_rvalid) begin
            fetch <= 1'b0;
            got_t <= fetch_t;
            word <= {rd_rword[7:0], rd_rword[15:8], rd_rword[23:16],
                     rd_rword[31:24]};
        end

        if (stb)
            pocket_sst_rdata <= addr[3:2] == 2'd0
                ? {28'd0, !w_queued, seen_s2, load_req, save_req}
                : 32'd0;
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_pocket_sst;
    always_comb unused_pocket_sst = ^{addr[27:4], addr[1:0], wdata[31:3],
                                      bridge_addr[1:0]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
