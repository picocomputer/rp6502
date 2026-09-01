/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The APF bridge met on our side of the fence. Slot words arrive on
 * clk_74a MSB-first — file byte zero rides wr_data[31:24] — and leave
 * as two halfword writes toward the SDRAM, even byte low.
 *
 * The machine's release is ordered by construction: the run gate rises
 * only with the SDRAM awake and the slot settled, and the length posts
 * two clocks after the rise because the machine's reset clears the
 * register it lands in. Completion is a level that clears only at the
 * next slot request, so the settle fires on its edge.
 *
 */

module pocket_bridge (
    input logic clk_74a,
    input logic arst_n,
    input logic bridge_wr,
    input logic [31:0] bridge_addr,
    input logic [31:0] bridge_wr_data,
    input logic dataslot_allcomplete,
    /* Host command 0x008A. Analogue documents it as the reload
     * announcement, and for a deferload slot that is what was measured.
     * For the non-deferload primary slot the host was measured doing
     * something else entirely: a hot reload is a request write, the
     * image rewritten at the first slot's address, the table entry, and
     * a second access-all-complete — no 0x008A anywhere in it. The
     * running machine hears about that reload through the settle below;
     * this pulse is still counted for the platforms and slots that do
     * announce the documented way. */
    input logic dataslot_update,
    input logic reset_n,
    output logic [9:0] pocket_bridge_dt_addr,
    output logic pocket_bridge_dt_busy,
    input logic [31:0] datatable_q,
    /* All four of APF's controller slots, in its own order. What each
     * one holds is the type nibble's to say and the firmware's to ask;
     * nothing here reads them. */
    input logic [3:0][31:0] cont_key,
    input logic [3:0][31:0] cont_joy,
    input logic [3:0][15:0] cont_trig,

    input logic clk_sys,
    input logic sdram_ready,
    input logic w_take,
    output logic pocket_bridge_w_avail,
    output logic [24:0] pocket_bridge_w_addr,
    output logic [15:0] pocket_bridge_w_data,
    output logic pocket_bridge_run,
    output logic pocket_bridge_slot_set,
    output logic [31:0] pocket_bridge_slot_len,
    /* Instrumentation. Counted where the events land, on the host's own
     * clock, so a count that rises while the machine saw nothing says
     * the loss is ours and not the host's silence. */
    output logic [7:0] pocket_bridge_upd_n,
    output logic [3:0][31:0] pocket_bridge_cont_key,
    output logic [3:0][31:0] pocket_bridge_cont_joy,
    output logic [3:0][15:0] pocket_bridge_cont_trig,
    /* The interact menu's persisted settings, as levels. */
    output logic [31:0] pocket_bridge_set_tz,
    output logic [31:0] pocket_bridge_set_tz_min,
    output logic [31:0] pocket_bridge_set_tz_sign,
    output logic [31:0] pocket_bridge_set_kb,

    /* The host's clock, written once at core boot by command 0x0090:
     * local wall time as seconds since 1970, and the flag that it was
     * ever written at all. */
    input logic [31:0] rtc_epoch,
    input logic rtc_valid,
    output logic [31:0] pocket_bridge_rtc_epoch,
    output logic pocket_bridge_rtc_valid
);

    /* --- Slot words into halfword writes, clk_74a side. --- */
    logic pend_second;
    logic [24:0] pend_addr;
    logic [15:0] pend_data;
    logic wf_full, wf_empty;
    logic wf_stb;
    logic [40:0] wf_wdata;
    logic slot_wr;
    always_comb slot_wr = bridge_wr && bridge_addr[31:28] == 4'h0;

    /* What the host named, held until the machine reads it. A pulse on
     * clk_74a, so it crosses as a toggle with the payload standing
     * beside it.
     *
     * The toggle has a hole and the counters below are how it gets
     * measured: two events closer together than the synchroniser is deep
     * flip it twice, the far side sees no change, and both are lost with
     * no trace. Whether that is what happens is exactly the question, so
     * upd_n counts the pulses here where they arrive and the machine
     * counts the deliveries at the other end. Two against none is this
     * module's fault; none against none is the host's silence. */
    logic upd_t;
    logic [7:0] upd_n, upd_g_74;
    always_ff @(posedge clk_74a or negedge arst_n) begin
        if (!arst_n) begin
            upd_t <= 1'b0;
            upd_n <= '0;
            upd_g_74 <= '0;
        end else if (dataslot_update) begin
            upd_t <= !upd_t;
            upd_n <= upd_n + 8'd1;
            /* Coded here and not at the far end. What crosses has to be
             * the Gray value already registered on this clock; converting
             * after the crossing samples the binary counter instead and
             * the coding buys nothing at all. */
            upd_g_74 <= b2g(upd_n + 8'd1);
        end
    end

    /* Gray, so a count read while it increments cannot come back as a
     * value it never held. One bit changes per step, so the worst a
     * sampler sees is the old number or the new one. */
    function automatic logic [7:0] b2g(input logic [7:0] b);
        return b ^ (b >> 1);
    endfunction
    function automatic logic [7:0] g2b(input logic [7:0] g);
        logic [7:0] b;
        b[7] = g[7];
        for (int i = 6; i >= 0; i--)
            b[i] = b[i+1] ^ g[i];
        return b;
    endfunction

    /* The menu's settings cross as levels, because that is what they
     * are. The host replays a persisted value once at load and then only
     * when the user changes it, so a mailbox would have nothing to hold
     * and an edge could be missed; a level the machine reads whenever it
     * likes cannot be. Two flops and an agreement, as the pad does.
     *
     * The time zone arrives in three pieces because the menu cannot
     * offer it in one. A list holds at most sixteen options and the
     * offset spans twenty-seven whole hours, so the sign, the hours and
     * the quarter-hour are three separate menu entries and three
     * separate registers. They could have shared one register — APF can
     * mask an element into part of a word — but that is a
     * read-modify-write, and these registers are write-only because the
     * bridge answers reads elsewhere. The firmware puts the pieces back
     * together. */
    logic [31:0] utz_74, utzm_74, utzs_74;
    /* The keyboard layout, by its position in def/keyboard.def plus one, so
     * a register nobody has written yet names no layout at all. */
    logic [31:0] kb_74;
    always_ff @(posedge clk_74a or negedge arst_n) begin
        if (!arst_n) begin
            utz_74   <= '0;
            utzm_74  <= '0;
            utzs_74  <= '0;
            kb_74    <= '0;
        end else if (bridge_wr) begin
            if (bridge_addr == 32'h1000_000C)
                utz_74 <= bridge_wr_data;
            if (bridge_addr == 32'h1000_0010)
                utzm_74 <= bridge_wr_data;
            if (bridge_addr == 32'h1000_0014)
                utzs_74 <= bridge_wr_data;
            if (bridge_addr == 32'h1000_0018)
                kb_74 <= bridge_wr_data;
        end
    end

    always_comb begin
        wf_stb = slot_wr || pend_second;
        wf_wdata = pend_second
            ? {pend_addr, pend_data}
            : {bridge_addr[25:1], bridge_wr_data[23:16],
               bridge_wr_data[31:24]};
    end

`ifdef VERILATOR
    always_ff @(posedge clk_74a) begin
        if (wf_stb && wf_full)
            $error("pocket_bridge: write fifo overflow");
        if (slot_wr && pend_second)
            $error("pocket_bridge: bridge outpaced the halfword split");
    end
`endif

    always_ff @(posedge clk_74a or negedge arst_n) begin
        if (!arst_n) begin
            pend_second <= 1'b0;
            pend_addr <= '0;
            pend_data <= '0;
        end else begin
            pend_second <= 1'b0;
            if (slot_wr) begin
                pend_second <= 1'b1;
                pend_addr <= {bridge_addr[25:2], 1'b1};
                pend_data <= {bridge_wr_data[7:0], bridge_wr_data[15:8]};
            end
        end
    end

    /* Eight, and the number is arithmetic rather than nerve.
     *
     * The queue has no backpressure to the host: w_stb is gated on
     * !wf_full, so an overrun is dropped rather than stalled, one
     * halfword of the image goes missing, and the load still reports
     * success. That is worth being careful about, and being careful
     * turned out to mean reading Analogue's own bridge peripheral:
     *
     *   "the worst-case read/write timing is every 88 cycles @ 74.25mhz
     *    which is about 1180ns."
     *
     * So the host cannot present a word faster than 1,185 ns. Each one
     * becomes two halfword entries here, so this fills at one entry per
     * 592 ns, and eight of them are 4.7 microseconds of tolerance. The
     * longest the controller can go without taking one is a self-refresh
     * wake, tXSR and all, at about 317 ns. Fifteen times the margin it
     * needs.
     *
     * It was two hundred and fifty six for a day, raised while chasing
     * ROM loads that failed one time in five — which turned out to be a
     * write-timing fault on the SRAM's port B and nothing to do with
     * this queue. The depth was defended on the grounds that the far
     * side could not be bounded. The far side is bounded, by the vendor,
     * in a comment, and it cost 10,496 bits of MLAB across twenty-four
     * LABs to not have read it. */
    pocket_fifo #(
        .WIDTH(41),
        .DEPTH_LOG2(3)
    ) wfifo (
        .wclk(clk_74a),
        .w_stb(wf_stb && !wf_full),
        .w_data(wf_wdata),
        .pocket_fifo_full(wf_full),
        .rclk(clk_sys),
        .r_take(w_take),
        .pocket_fifo_empty(wf_empty),
        .pocket_fifo_rdata({pocket_bridge_w_addr, pocket_bridge_w_data})
    );
    always_comb pocket_bridge_w_avail = !wf_empty;

    /* --- The slot settling, clk_74a side. ---
     * Completion or a host reset-exit rereads the table; the size is
     * quasi-static behind its toggle. */
    logic reset_n_q;
    logic allcomplete_q;
    logic [1:0] dt_read;
    logic [31:0] slot_size;
    logic settle_t;
    /* The word this reads is fixed, so sharing the table's one port with
     * pocket_file costs nothing but the right of way: this read starts
     * on an edge and cannot be asked to wait, so it says when it holds
     * the address and the other side stands off.
     *
     * Word 1 is the size beside id 0, which is the ROM — the primary
     * slot, because a hot reload was measured writing the new image
     * through the first slot record and nowhere else. Analogue
     * documents the table as 32 entries of eight bytes, word 0 the slot
     * id and word 1 the size, and a probe confirmed it on hardware:
     * the sizes stand beside their own ids, entry index equal to id for
     * as long as data.json lists the slots in id order. The firmware
     * scans for the id anyway, because a scan costs nothing at boot and
     * does not care whether a slot is ever renumbered again.
     *
     * Boot fires this read, and so does a hot reload: the request write
     * drops completion, the image and the table entry arrive, and the
     * closing access-all-complete is a fresh edge. The size posts to
     * the machine mid-run, and that post is the whole announcement. */
    logic dt_trig;
    always_comb begin
        pocket_bridge_dt_addr = 10'd1;
        dt_trig = (dataslot_allcomplete && !allcomplete_q)
            || (reset_n && !reset_n_q);
        pocket_bridge_dt_busy = dt_trig || |dt_read;
    end
    always_ff @(posedge clk_74a or negedge arst_n) begin
        if (!arst_n) begin
            reset_n_q <= 1'b0;
            allcomplete_q <= 1'b0;
            dt_read <= '0;
            slot_size <= '0;
            settle_t <= 1'b0;
        end else begin
            reset_n_q <= reset_n;
            allcomplete_q <= dataslot_allcomplete;
            dt_read <= {dt_read[0], 1'b0};
            if (dt_trig)
                dt_read[0] <= 1'b1;
            if (dt_read[1]) begin
                slot_size <= datatable_q;
                settle_t <= !settle_t;
            end
        end
    end

    /* --- The machine's release and the posting, clk_sys side. --- */
    /* Preserved: two flops in series with nothing between them are
     * equivalent, and without a reset to tell them apart the fitter
     * merges them and the crossing loses its synchroniser. */
    (* preserve *) logic settle_t1, settle_t2, settle_t3;
    (* preserve *) logic [7:0] upd_g_s1;
    logic [7:0] upd_g_s2, upd_n_sys;
    (* preserve *) logic reset_n_s1, reset_n_s2;
    logic settled;
    logic run_q;
    logic [3:0] post;
    initial begin
        settle_t1 = 1'b0;
        settle_t2 = 1'b0;
        settle_t3 = 1'b0;
        upd_g_s1 = '0;
        upd_g_s2 = '0;
        upd_n_sys = '0;
        reset_n_s1 = 1'b0;
        reset_n_s2 = 1'b0;
        settled = 1'b0;
        run_q = 1'b0;
        post = '0;
        pocket_bridge_slot_len = '0;
        pocket_bridge_slot_set = 1'b0;
    end
    always_ff @(posedge clk_sys) begin
        settle_t1 <= settle_t;
        settle_t2 <= settle_t1;
        settle_t3 <= settle_t2;
        upd_g_s1 <= upd_g_74;
        upd_g_s2 <= upd_g_s1;
        /* Decoded into a register rather than into the read path: the
         * conversion is a seven-deep XOR chain and the machine's MMIO
         * mux has no room for it. */
        upd_n_sys <= g2b(upd_g_s2);
        reset_n_s1 <= reset_n;
        reset_n_s2 <= reset_n_s1;
        run_q <= pocket_bridge_run;
        pocket_bridge_slot_set <= 1'b0;
        post <= {post[2:0], 1'b0};
        if (settle_t2 != settle_t3) begin
            settled <= 1'b1;
            pocket_bridge_slot_len <= slot_size;
        end
        /* Post after the rise — the machine's reset just cleared
         * the register — and on a fresh size while running. */
        if ((pocket_bridge_run && !run_q)
            || (settle_t2 != settle_t3 && pocket_bridge_run))
            post[0] <= 1'b1;
        if (post[3])
            pocket_bridge_slot_set <= 1'b1;
    end
    always_comb pocket_bridge_run = reset_n_s2 && settled && sdram_ready;

    always_comb pocket_bridge_upd_n = upd_n_sys;

    /* --- The controller slots. ---
     *
     * They cross as state, because state is what they are: a level
     * needs no mailbox and would be wrong in one, losing the release
     * that a game reads as a held button. Whole words cross on two
     * flops the way the buttons always did, and land only when two
     * consecutive samples agree, so a word never carries half of one
     * poll and half of the next. APF repolls every 882 us, which is
     * hundreds of machine clocks between changes, so agreeing costs
     * nothing and tearing an axis would be visible.
     *
     * Nothing here reads what it carries. A gamepad, the dock's
     * keyboard and the dock's mouse all arrive this way and are told
     * apart by the type nibble APF puts in the top of each key word,
     * which is the firmware's business — so all four slots cross
     * alike and none of them is named for a device. */
    /* Preserved, every one of them. These are the two flops that make a
     * crossing safe, and two flops in series with nothing between them
     * are equivalent — with a reset to tell them apart the fitter left
     * them alone, and without one it merged rv_s1 and rv_s2 into the
     * register they feed. What was left was clk_74a reaching a clk_sys
     * flop directly: no synchroniser, and a path the analyzer then
     * reported as six nanoseconds of setup failure. */
    (* preserve *) logic [3:0][31:0] ck_s1, ck_s2, cj_s1, cj_s2;
    (* preserve *) logic [3:0][15:0] ct_s1, ct_s2;
    (* preserve *) logic [31:0] ut_s1, ut_s2;
    (* preserve *) logic [31:0] um_s1, um_s2, us_s1, us_s2;
    (* preserve *) logic [31:0] kb_s1, kb_s2;
    (* preserve *) logic [31:0] re_s1, re_s2;
    (* preserve *) logic rv_s1, rv_s2;
    initial begin
        ck_s1 = '0; ck_s2 = '0;
        cj_s1 = '0; cj_s2 = '0;
        ct_s1 = '0; ct_s2 = '0;
        ut_s1 = '0; ut_s2 = '0;
        um_s1 = '0; um_s2 = '0;
        us_s1 = '0; us_s2 = '0;
        kb_s1 = '0; kb_s2 = '0;
        re_s1 = '0; re_s2 = '0;
        rv_s1 = 1'b0; rv_s2 = 1'b0;
        pocket_bridge_set_tz = '0;
        pocket_bridge_set_tz_min = '0;
        pocket_bridge_set_tz_sign = '0;
        pocket_bridge_set_kb = '0;
        pocket_bridge_rtc_epoch = '0;
        pocket_bridge_rtc_valid = 1'b0;
        pocket_bridge_cont_key = '0;
        pocket_bridge_cont_joy = '0;
        pocket_bridge_cont_trig = '0;
    end
    always_ff @(posedge clk_sys) begin
        ck_s1 <= cont_key;  ck_s2 <= ck_s1;
        cj_s1 <= cont_joy;  cj_s2 <= cj_s1;
        ct_s1 <= cont_trig; ct_s2 <= ct_s1;
        for (int s = 0; s < 4; s++) begin
            if (ck_s1[s] == ck_s2[s]) pocket_bridge_cont_key[s] <= ck_s2[s];
            if (cj_s1[s] == cj_s2[s]) pocket_bridge_cont_joy[s] <= cj_s2[s];
            if (ct_s1[s] == ct_s2[s]) pocket_bridge_cont_trig[s] <= ct_s2[s];
        end
        ut_s1 <= utz_74;   ut_s2 <= ut_s1;
        um_s1 <= utzm_74;  um_s2 <= um_s1;
        us_s1 <= utzs_74;  us_s2 <= us_s1;
        kb_s1 <= kb_74;    kb_s2 <= kb_s1;
        re_s1 <= rtc_epoch; re_s2 <= re_s1;
        rv_s1 <= rtc_valid; rv_s2 <= rv_s1;
        if (ut_s1 == ut_s2) pocket_bridge_set_tz <= ut_s2;
        if (um_s1 == um_s2) pocket_bridge_set_tz_min <= um_s2;
        if (us_s1 == us_s2) pocket_bridge_set_tz_sign <= us_s2;
        if (kb_s1 == kb_s2) pocket_bridge_set_kb <= kb_s2;
        if (re_s1 == re_s2) pocket_bridge_rtc_epoch <= re_s2;
        pocket_bridge_rtc_valid <= rv_s2;
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_pocket_bridge;
    always_comb unused_pocket_bridge = ^{bridge_addr[27:26]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
