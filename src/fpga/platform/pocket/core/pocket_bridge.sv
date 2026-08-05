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
    /* Host command 0x008A, and for a user-reloadable slot whose
     * parameters leave bit 6 clear it is the whole announcement: Analogue
     * documents that APF then sends this instead of a request write and
     * an access-all-complete, and without bit 6 there is no Reset Enter
     * and Exit either. So the machine keeps running and this is the only
     * thing that tells it the user picked a different file.
     *
     * It arrives on the host's clock as a pulse. The payload crosses on a
     * toggle beside it, which two announcements close together would
     * cancel, so the count below is what the firmware watches. */
    input logic dataslot_update,
    input logic [15:0] dataslot_update_id,
    input logic [31:0] dataslot_update_size,
    input logic reset_n,
    output logic [9:0] pocket_bridge_dt_addr,
    output logic pocket_bridge_dt_busy,
    input logic [31:0] datatable_q,
    input logic [31:0] cont1_key,
    input logic [31:0] cont1_joy,
    input logic [15:0] cont1_trig,
    input logic [31:0] cont3_key,
    input logic [31:0] cont3_joy,
    input logic [15:0] cont3_trig,
    input logic [31:0] cont4_key,
    input logic [31:0] cont4_joy,
    input logic [15:0] cont4_trig,

    input logic clk_sys,
    input logic sdram_ready,
    input logic w_take,
    output logic pocket_bridge_w_avail,
    output logic [24:0] pocket_bridge_w_addr,
    output logic [15:0] pocket_bridge_w_data,
    output logic pocket_bridge_run,
    output logic pocket_bridge_slot_set,
    output logic [31:0] pocket_bridge_slot_len,
    output logic pocket_bridge_upd_set,
    output logic [15:0] pocket_bridge_upd_id,
    output logic [31:0] pocket_bridge_upd_len,
    /* Instrumentation. Counted where the events land, on the host's own
     * clock, so a count that rises while the machine saw nothing says
     * the loss is ours and not the host's silence. */
    output logic [7:0] pocket_bridge_upd_n,
    output logic [31:0] pocket_bridge_pad_key,
    output logic [31:0] pocket_bridge_pad_joy,
    output logic [15:0] pocket_bridge_pad_trig,
    output logic [31:0] pocket_bridge_kbd_key,
    output logic [31:0] pocket_bridge_kbd_joy,
    output logic [15:0] pocket_bridge_kbd_trig,
    output logic [31:0] pocket_bridge_mou_key,
    output logic [31:0] pocket_bridge_mou_joy,
    output logic [15:0] pocket_bridge_mou_trig,
    /* The interact menu's persisted settings, as levels. */
    output logic [31:0] pocket_bridge_set_tz,
    output logic [31:0] pocket_bridge_set_tz_min,
    output logic [31:0] pocket_bridge_set_tz_sign,

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
    logic [15:0] upd_id_74;
    logic [31:0] upd_len_74;
    logic upd_t;
    logic [7:0] upd_n;
    always_ff @(posedge clk_74a or negedge arst_n) begin
        if (!arst_n) begin
            upd_id_74 <= '0;
            upd_len_74 <= '0;
            upd_t <= 1'b0;
            upd_n <= '0;
        end else if (dataslot_update) begin
            upd_id_74 <= dataslot_update_id;
            upd_len_74 <= dataslot_update_size;
            upd_t <= !upd_t;
            upd_n <= upd_n + 8'd1;
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
    always_ff @(posedge clk_74a or negedge arst_n) begin
        if (!arst_n) begin
            utz_74   <= '0;
            utzm_74  <= '0;
            utzs_74  <= '0;
        end else if (bridge_wr) begin
            if (bridge_addr == 32'h1000_000C)
                utz_74 <= bridge_wr_data;
            if (bridge_addr == 32'h1000_0010)
                utzm_74 <= bridge_wr_data;
            if (bridge_addr == 32'h1000_0014)
                utzs_74 <= bridge_wr_data;
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

    /* Two hundred and fifty six, and the depth is a safety margin rather
     * than a speed one.
     *
     * This queue has no backpressure to the host. APF's bridge writes
     * arrive at the host's pace and w_stb is gated on !wf_full, so an
     * overrun is not stalled — it is dropped, one halfword of the image
     * goes missing, the SDRAM keeps whatever was at that address, and the
     * only complaint is an $error that exists in simulation alone. On
     * hardware the load reports success and the program is quietly wrong,
     * which is the worst shape a fault can have here.
     *
     * So the depth has to cover the longest the controller can go without
     * accepting a word, and that is not a number this side can bound: a
     * refresh, a self-refresh wake with its tXSR, and a row miss all
     * stack, and a file read streams into this same queue while a program
     * runs. Eight was never shown to be enough — it was only never seen
     * to fail in a bench that peaks at two entries.
     *
     * It was raised while chasing ROM loads that failed one time in five.
     * That turned out to be a write-timing fault on the SRAM's port B and
     * had nothing to do with this queue, so nothing here is evidence for
     * the depth. It stays because the failure it guards against is silent
     * and the guard is cheap, not because it ever caught anything. */
    pocket_fifo #(
        .WIDTH(41),
        .DEPTH_LOG2(8)
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
     * Word 17 is the size beside id 8, which is the ROM. Analogue
     * documents the table as 32 entries of eight bytes, word 0 the slot
     * id and word 1 the size, and a probe confirmed it on hardware:
     * 0xF000 and 0x13D8 stand beside ids 9 and 10, exactly the two
     * assets' size_exact. The firmware scans for the id anyway, because
     * a scan costs nothing at boot and does not care whether a slot is
     * ever renumbered again.
     *
     * This read is boot's alone now. A user-reloadable slot without bit
     * 6 gets no Reset Exit and no completion when its file changes, so
     * nothing re-triggers it; a running machine hears about a change
     * through the update count instead. */
    logic dt_trig;
    always_comb begin
        pocket_bridge_dt_addr = 10'd17;
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
    (* preserve *) logic upd_t1, upd_t2, upd_t3;
    (* preserve *) logic [7:0] g_upd1;
    logic [7:0] g_upd2;
    (* preserve *) logic reset_n_s1, reset_n_s2;
    logic settled;
    logic run_q;
    logic [3:0] post;
    initial begin
        settle_t1 = 1'b0;
        settle_t2 = 1'b0;
        settle_t3 = 1'b0;
        upd_t1 = 1'b0;
        upd_t2 = 1'b0;
        upd_t3 = 1'b0;
        g_upd1 = '0;
        g_upd2 = '0;
        pocket_bridge_upd_set = 1'b0;
        pocket_bridge_upd_id = '0;
        pocket_bridge_upd_len = '0;
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
        upd_t1 <= upd_t;
        upd_t2 <= upd_t1;
        upd_t3 <= upd_t2;
        g_upd1 <= b2g(upd_n);
        g_upd2 <= g_upd1;
        pocket_bridge_upd_set <= 1'b0;
        if (upd_t2 != upd_t3) begin
            pocket_bridge_upd_set <= 1'b1;
            pocket_bridge_upd_id <= upd_id_74;
            pocket_bridge_upd_len <= upd_len_74;
        end
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

    always_comb pocket_bridge_upd_n = g2b(g_upd2);

    /* --- The controller, and the dock's keyboard. ---
     *
     * The pad crosses as state, because state is what it is: a level
     * needs no mailbox and would be wrong in one, losing the release
     * that a game reads as a held button. Whole words cross on two
     * flops the way the buttons always did, and land only when two
     * consecutive samples agree, so a word never carries half of one
     * poll and half of the next. APF repolls every 882 us, which is
     * hundreds of machine clocks between changes, so agreeing costs
     * nothing and tearing an axis would be visible.
     *
     * Nothing here turns a button into a key any more. The machine has
     * had a gamepad since it was designed and did not need the pad to
     * pretend; the keys below are the dock's own keyboard, arriving as
     * scan codes on the third slot the way APF sends them. */
    /* Preserved, every one of them. These are the two flops that make a
     * crossing safe, and two flops in series with nothing between them
     * are equivalent — with a reset to tell them apart the fitter left
     * them alone, and without one it merged rv_s1 and rv_s2 into the
     * register they feed. What was left was clk_74a reaching a clk_sys
     * flop directly: no synchroniser, and a path the analyzer then
     * reported as six nanoseconds of setup failure. */
    (* preserve *) logic [31:0] pk_s1, pk_s2, pj_s1, pj_s2;
    (* preserve *) logic [31:0] kk_s1, kk_s2, kj_s1, kj_s2;
    (* preserve *) logic [31:0] mk_s1, mk_s2, mj_s1, mj_s2;
    (* preserve *) logic [31:0] ut_s1, ut_s2;
    (* preserve *) logic [31:0] um_s1, um_s2, us_s1, us_s2;
    (* preserve *) logic [31:0] re_s1, re_s2;
    (* preserve *) logic rv_s1, rv_s2;
    (* preserve *) logic [15:0] pt_s1, pt_s2, kt_s1, kt_s2, mt_s1, mt_s2;
    initial begin
        pk_s1 = '0; pk_s2 = '0;
        pj_s1 = '0; pj_s2 = '0;
        pt_s1 = '0; pt_s2 = '0;
        kk_s1 = '0; kk_s2 = '0;
        kj_s1 = '0; kj_s2 = '0;
        kt_s1 = '0; kt_s2 = '0;
        ut_s1 = '0; ut_s2 = '0;
        um_s1 = '0; um_s2 = '0;
        us_s1 = '0; us_s2 = '0;
        re_s1 = '0; re_s2 = '0;
        rv_s1 = 1'b0; rv_s2 = 1'b0;
        pocket_bridge_set_tz = '0;
        pocket_bridge_set_tz_min = '0;
        pocket_bridge_set_tz_sign = '0;
        pocket_bridge_rtc_epoch = '0;
        pocket_bridge_rtc_valid = 1'b0;
        mk_s1 = '0; mk_s2 = '0;
        mj_s1 = '0; mj_s2 = '0;
        mt_s1 = '0; mt_s2 = '0;
        pocket_bridge_mou_key = '0;
        pocket_bridge_mou_joy = '0;
        pocket_bridge_mou_trig = '0;
        pocket_bridge_pad_key = '0;
        pocket_bridge_pad_joy = '0;
        pocket_bridge_pad_trig = '0;
        pocket_bridge_kbd_key = '0;
        pocket_bridge_kbd_joy = '0;
        pocket_bridge_kbd_trig = '0;
    end
    always_ff @(posedge clk_sys) begin
        pk_s1 <= cont1_key;  pk_s2 <= pk_s1;
        pj_s1 <= cont1_joy;  pj_s2 <= pj_s1;
        pt_s1 <= cont1_trig; pt_s2 <= pt_s1;
        kk_s1 <= cont3_key;  kk_s2 <= kk_s1;
        kj_s1 <= cont3_joy;  kj_s2 <= kj_s1;
        kt_s1 <= cont3_trig; kt_s2 <= kt_s1;
        if (pk_s1 == pk_s2) pocket_bridge_pad_key <= pk_s2;
        if (pj_s1 == pj_s2) pocket_bridge_pad_joy <= pj_s2;
        if (pt_s1 == pt_s2) pocket_bridge_pad_trig <= pt_s2;
        if (kk_s1 == kk_s2) pocket_bridge_kbd_key <= kk_s2;
        if (kj_s1 == kj_s2) pocket_bridge_kbd_joy <= kj_s2;
        if (kt_s1 == kt_s2) pocket_bridge_kbd_trig <= kt_s2;
        mk_s1 <= cont4_key;  mk_s2 <= mk_s1;
        mj_s1 <= cont4_joy;  mj_s2 <= mj_s1;
        mt_s1 <= cont4_trig; mt_s2 <= mt_s1;
        ut_s1 <= utz_74;   ut_s2 <= ut_s1;
        um_s1 <= utzm_74;  um_s2 <= um_s1;
        us_s1 <= utzs_74;  us_s2 <= us_s1;
        re_s1 <= rtc_epoch; re_s2 <= re_s1;
        rv_s1 <= rtc_valid; rv_s2 <= rv_s1;
        if (ut_s1 == ut_s2) pocket_bridge_set_tz <= ut_s2;
        if (um_s1 == um_s2) pocket_bridge_set_tz_min <= um_s2;
        if (us_s1 == us_s2) pocket_bridge_set_tz_sign <= us_s2;
        if (re_s1 == re_s2) pocket_bridge_rtc_epoch <= re_s2;
        pocket_bridge_rtc_valid <= rv_s2;
        if (mk_s1 == mk_s2) pocket_bridge_mou_key <= mk_s2;
        if (mj_s1 == mj_s2) pocket_bridge_mou_joy <= mj_s2;
        if (mt_s1 == mt_s2) pocket_bridge_mou_trig <= mt_s2;
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_pocket_bridge;
    always_comb unused_pocket_bridge = ^{bridge_addr[27:26]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
