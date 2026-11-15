/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The ack and status signals of the host's savestate commands are
 * driven here and not by the firmware, because sst_engine halts the
 * soft CPU at its debug port while a blob is made or loaded.
 *
 * core_bridge_cmd holds a request line high until it sees the ack, and
 * the result it gives the host is the status lines in the cycle of the
 * ack. The ack is therefore one clock after the request's rising edge,
 * when the status registers already describe the new request.
 *
 * A save answers busy until the blob is ready, then ok, or err once a
 * word was not served in time. A load asked for while the machine runs
 * answers busy until the engine has finished, then ok or err.
 *
 * The firmware holds saves off while a file is open for writing, in
 * REG_REFUSE. A save asked for then answers err at once, the engine is
 * not asked and the machine runs on.
 *
 * A wake's load can arrive while the host still holds the core in
 * reset, where the engine cannot run, and the host may wait for the
 * answer before it sends Reset Exit. Such a load answers ok at once and
 * keeps answering ok. The request is kept, the engine performs it when
 * the reset is released, and the firmware reads how it went in SST_CTL.
 * A blob the engine then refuses leaves a machine that starts its
 * program over, which is a machine the host can go on with.
 *
 * A request that arrives while a load is with the engine or the
 * firmware is acknowledged and answered with that load's status, and
 * starts nothing.
 */

module pocket_sst #(
    /* BLOB_BASE and BLOB_WINDOW are core_top's savestate_addr and
     * savestate_maxloadsize. The window is 512 KB at a multiple of
     * 512 KB, so the decode compares bits 31:19 with the base. */
    parameter logic [31:0] BLOB_BASE = 32'h03E8_0000,
    parameter logic [31:0] BLOB_WINDOW = 32'h0008_0000,
    /* BLOB_WORDS is sst_engine's word count and core_top's
     * savestate_size divided by four, and stage_map_gate.py checks that
     * the three agree. No host command marks the end of reading the
     * blob, so the save request is dropped when the word at the last
     * index arrives from sst_engine. */
    parameter int BLOB_WORDS = 75092
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
    /* core_bridge_cmd's reset_n, low until the host's Reset Exit. */
    input logic reset_n,
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
    input logic [2:0] sst_load_why,

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
    /* The firmware writes bit 0 set to have saves refused and clear to
     * allow them. Bit 0 reads set once the refusal is in force with no
     * save in progress, and no save begins from then until the firmware
     * writes it clear. */
    localparam logic [1:0] REG_REFUSE = 2'd1;
    /* What a write to REG_CTL does, a bit each. CTL_DONE ends the load the
     * firmware has finished with, CTL_SEEN drops a blob no load was asked
     * for, and CTL_WRITING clears the mark a blob write leaves. */
    localparam int CTL_DONE = 0;
    localparam int CTL_SEEN = 1;
    localparam int CTL_WRITING = 6;

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
        in_window = bridge_addr[31:19] == BLOB_BASE[31:19];
        blob_hit = bridge_wr && in_window;
        rd_idx = {1'b0, bridge_addr[18:2]};
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
    /* refuse_s2 is the firmware's refusal on this clock. held is the
     * refusal with no save begun, which then stays so while refuse_s2
     * does: a request that finds refuse_s2 set begins no save. refused
     * marks the last request as one turned away. */
    (* preserve *) logic refuse_s1, refuse_s2;
    logic held, refused;
    /* load_act is high from a load's request until the firmware has
     * finished with it, and load_fin from the engine's done until then.
     * load_early marks a load asked for in reset. */
    logic load_act, load_fin, load_early, load_ok, load_bad;
    logic load_t;
    /* wr_t toggles on every write into the window. unasked_q is a blob
     * in the window that no load has been asked for. */
    logic wr_t, unasked_q;
    (* preserve *) logic drop_s1, drop_s2, drop_s3;

    always_comb begin
        pocket_sst_start_ack = savestate_start && start_q;
        pocket_sst_load_ack = savestate_load && load_q;
        pocket_sst_start_busy = start_hold && !start_done;
        pocket_sst_start_ok = start_done && !late;
        pocket_sst_start_err = (start_done && late) || refused;
        pocket_sst_load_busy = load_act && !load_ok && !load_bad;
        pocket_sst_load_ok = load_ok;
        pocket_sst_load_err = load_bad;
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
            load_act <= 1'b0;
            load_fin <= 1'b0;
            load_early <= 1'b0;
            load_ok <= 1'b0;
            load_bad <= 1'b0;
            load_t <= 1'b0;
            wr_t <= 1'b0;
            unasked_q <= 1'b0;
            drop_s1 <= 1'b0;
            drop_s2 <= 1'b0;
            drop_s3 <= 1'b0;
            refuse_s1 <= 1'b0;
            refuse_s2 <= 1'b0;
            held <= 1'b0;
            refused <= 1'b0;
        end else begin
            refuse_s1 <= refuse_req;
            refuse_s2 <= refuse_s1;
            held <= refuse_s2 && !start_hold;
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
            drop_s1 <= drop_t;
            drop_s2 <= drop_s1;
            drop_s3 <= drop_s2;
            unasked_q <= blob_seen && !load_act;

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

            /* The host writes a blob into the window before or after it
             * sends the load command, so either is the first sign of a
             * restore. The firmware reads blob_seen as a restore to wait
             * for, so it is cleared when the engine has finished the
             * load, or by the firmware when no load has been asked for.
             * The firmware's drop comes first here, so a blob written or
             * a load asked for on its clock is kept. */
            if (drop_s2 != drop_s3 && !load_act) blob_seen <= 1'b0;
            if (load_act && ldone_s2 && !load_fin) begin
                load_fin <= 1'b1;
                blob_seen <= 1'b0;
                if (!load_early) begin
                    load_ok <= !lerr_s2;
                    load_bad <= lerr_s2;
                end
            end
            /* The engine's done falls when the firmware has cleared the
             * request. The load is over, so a blob the host wrote again
             * while it ran is no new wake. */
            if (load_fin && !ldone_s2) begin
                load_act <= 1'b0;
                load_fin <= 1'b0;
                blob_seen <= 1'b0;
            end
            if (blob_hit) begin
                blob_seen <= 1'b1;
                wr_t <= !wr_t;
            end

            if (start_hold && ready_s2) start_done <= 1'b1;

            /* A request the firmware has refused begins nothing and is
             * the last request's result. A request while a save's blob is
             * still held starts no second save. It reads the same blob
             * again from the start. */
            if (savestate_start && !start_q) begin
                if (refuse_s2 && !start_hold) begin
                    refused <= 1'b1;
                    start_done <= 1'b0;
                end else begin
                    refused <= 1'b0;
                    start_hold <= 1'b1;
                    if (!start_hold) start_done <= 1'b0;
                    asked <= 1'b0;
                    hold_valid <= 1'b0;
                    seen_low <= 1'b0;
                end
                late <= 1'b0;
            end
            if (savestate_load && !load_q && !load_act) begin
                load_act <= 1'b1;
                load_fin <= 1'b0;
                load_early <= !reset_n;
                load_ok <= !reset_n;
                load_bad <= 1'b0;
                load_t <= !load_t;
                blob_seen <= 1'b1;
            end
        end
    end

    logic load_req;
    logic drop_t, writing;
    (* preserve *) logic load_s1, load_s2, load_s3;
    (* preserve *) logic seen_s1, seen_s2;
    (* preserve *) logic under_s1, under_s2;
    (* preserve *) logic early_s1, early_s2;
    (* preserve *) logic unasked_s1, unasked_s2;
    (* preserve *) logic wr_s1, wr_s2, wr_s3;
    /* refuse_want is what the firmware last wrote. refuse_req, which
     * crosses, takes it only once the value before has come back in
     * echo, so no two changes are on their way at once and an echo that
     * agrees with refuse_req is that value's own. */
    logic refuse_want, refuse_req;
    (* preserve *) logic echo_s1, echo_s2, echo_s3;
    (* preserve *) logic held_s1, held_s2;

    initial begin
        pocket_sst_rdata = '0;
        load_req = 1'b0;
        drop_t = 1'b0;
        writing = 1'b0;
        load_s1 = 1'b0;
        load_s2 = 1'b0;
        load_s3 = 1'b0;
        seen_s1 = 1'b0;
        seen_s2 = 1'b0;
        under_s1 = 1'b0;
        under_s2 = 1'b0;
        early_s1 = 1'b0;
        early_s2 = 1'b0;
        unasked_s1 = 1'b0;
        unasked_s2 = 1'b0;
        wr_s1 = 1'b0;
        wr_s2 = 1'b0;
        wr_s3 = 1'b0;
        refuse_want = 1'b0;
        refuse_req = 1'b0;
        echo_s1 = 1'b0;
        echo_s2 = 1'b0;
        echo_s3 = 1'b0;
        held_s1 = 1'b0;
        held_s2 = 1'b0;
    end

    always_comb pocket_sst_load = load_req;

    logic ctl_we;
    always_comb ctl_we = stb && we && addr[3:2] == REG_CTL;

    /* held crosses one clk_74a cycle behind the refusal it was made
     * from. echo_s3 is a cycle behind echo_s2, so held_s2 is of this
     * refusal and not left from the one before. */
    logic refusing;
    always_comb
        refusing = refuse_want && refuse_req && echo_s2 && echo_s3 && held_s2;

    always_ff @(posedge clk_sys) begin
        load_s1 <= load_t;
        load_s2 <= load_s1;
        load_s3 <= load_s2;
        seen_s1 <= blob_seen;
        seen_s2 <= seen_s1;
        under_s1 <= late;
        under_s2 <= under_s1;
        early_s1 <= load_early;
        early_s2 <= early_s1;
        unasked_s1 <= unasked_q;
        unasked_s2 <= unasked_s1;
        wr_s1 <= wr_t;
        wr_s2 <= wr_s1;
        wr_s3 <= wr_s2;
        echo_s1 <= refuse_s2;
        echo_s2 <= echo_s1;
        echo_s3 <= echo_s2;
        held_s1 <= held;
        held_s2 <= held_s1;

        if (stb && we && addr[3:2] == REG_REFUSE) refuse_want <= wdata[0];
        if (echo_s2 == refuse_req) refuse_req <= refuse_want;

        if (ctl_we && wdata[CTL_DONE]) load_req <= 1'b0;
        if (ctl_we && wdata[CTL_SEEN]) drop_t <= !drop_t;
        if (ctl_we && wdata[CTL_WRITING]) writing <= 1'b0;

        /* These come after the clears, so a load request or a blob write
         * that arrives in the same cycle as the firmware's clear is
         * kept. load_req has no reset: a load asked for in reset is
         * still here when the engine leaves it. */
        if (load_s2 != load_s3) load_req <= 1'b1;
        if (wr_s2 != wr_s3) writing <= 1'b1;
        /* A blob with no load asked for reads as written on the clock it
         * first reads as unasked, so the firmware never sees it unasked
         * and at rest before it has seen it arrive. */
        if (unasked_s1 && !unasked_s2) writing <= 1'b1;

        /* A read of bit 0 returns sst_load_done and not load_req, so the
         * firmware reads it as set only after sst_engine has finished the
         * load and released the soft CPU. After a successful load the 6502
         * stays in reset until the firmware writes 1 to bit 0. */
        if (stb)
            case (addr[3:2])
                REG_CTL: pocket_sst_rdata <=
                    {21'd0, sst_load_why, 1'b0, writing, unasked_s2,
                     early_s2, sst_load_err, under_s2, seen_s2, sst_load_done};
                REG_REFUSE: pocket_sst_rdata <= {31'd0, refusing};
                default: pocket_sst_rdata <= 32'd0;
            endcase
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_pocket_sst;
    always_comb unused_pocket_sst = we ^ (^addr[27:4]) ^ (^addr[1:0])
        ^ (^wdata[31:7]) ^ (^wdata[5:2]) ^ (^bridge_addr[1:0]) ^ ready_s1
        ^ (^BLOB_BASE[18:0]) ^ (^BLOB_WINDOW);
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
