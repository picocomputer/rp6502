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
 * A write into the 0x1 window dips the run gate: the interact menu's
 * "Reset 6502", which reloads the ROM still staged in the SDRAM.
 */

module pocket_bridge (
    input logic clk_74a,
    input logic arst_n,
    input logic bridge_wr,
    input logic [31:0] bridge_addr,
    input logic [31:0] bridge_wr_data,
    input logic dataslot_allcomplete,
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
    input logic rst_n,
    input logic sdram_ready,
    input logic w_take,
    output logic pocket_bridge_w_avail,
    output logic [24:0] pocket_bridge_w_addr,
    output logic [15:0] pocket_bridge_w_data,
    output logic pocket_bridge_run,
    output logic pocket_bridge_slot_set,
    output logic [31:0] pocket_bridge_slot_len,
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
    output logic [31:0] pocket_bridge_set_phi2,
    output logic [31:0] pocket_bridge_set_cp,
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

    /* The interact menu. The action is decoded on its whole address, not
     * on the nibble: every variable in the menu writes somewhere in this
     * range, and matching the range alone would reboot the machine each
     * time a setting moved.
     *
     * The settings themselves cross as levels, because that is what they
     * are. The host replays a persisted value once at load and then only
     * when the user changes it, so a mailbox would have nothing to hold
     * and an edge could be missed; a level the machine reads whenever it
     * likes cannot be. Two flops and an agreement, as the pad does. */
    logic urst_t;
    always_ff @(posedge clk_74a or negedge arst_n) begin
        if (!arst_n)
            urst_t <= 1'b0;
        else if (bridge_wr && bridge_addr == 32'h1000_0000)
            urst_t <= !urst_t;
    end

    /* The time zone arrives in three pieces because the menu cannot
     * offer it in one. A list holds at most sixteen options and the
     * offset spans twenty-seven whole hours, so the sign, the hours and
     * the quarter-hour are three separate menu entries and three
     * separate registers. They could have shared one register — APF can
     * mask an element into part of a word — but that is a
     * read-modify-write, and these registers are write-only because the
     * bridge answers reads elsewhere. The firmware puts the pieces back
     * together. */
    logic [31:0] uphi2_74, ucp_74, utz_74, utzm_74, utzs_74;
    always_ff @(posedge clk_74a or negedge arst_n) begin
        if (!arst_n) begin
            uphi2_74 <= '0;
            ucp_74   <= '0;
            utz_74   <= '0;
            utzm_74  <= '0;
            utzs_74  <= '0;
        end else if (bridge_wr) begin
            if (bridge_addr == 32'h1000_0004)
                uphi2_74 <= bridge_wr_data;
            if (bridge_addr == 32'h1000_0008)
                ucp_74 <= bridge_wr_data;
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

    pocket_fifo #(
        .WIDTH(41),
        .DEPTH_LOG2(3)
    ) wfifo (
        .wclk(clk_74a),
        .wrst_n(arst_n),
        .w_stb(wf_stb && !wf_full),
        .w_data(wf_wdata),
        .pocket_fifo_full(wf_full),
        .rclk(clk_sys),
        .rrst_n(rst_n),
        .r_take(w_take),
        .pocket_fifo_empty(wf_empty),
        .pocket_fifo_rdata({pocket_bridge_w_addr, pocket_bridge_w_data})
    );
    always_comb pocket_bridge_w_avail = !wf_empty;

    /* --- The slot settling, clk_74a side. ---
     * Completion or a host reset-exit rereads the table; the size is
     * quasi-static behind its toggle. Word 1 carries slot 0's size. */
    logic reset_n_q;
    logic allcomplete_q;
    logic [1:0] dt_read;
    logic [31:0] slot_size;
    logic settle_t;
    /* The word this reads is fixed, so sharing the table's one port with
     * pocket_file costs nothing but the right of way: this read starts
     * on an edge and cannot be asked to wait, so it says when it holds
     * the address and the other side stands off. */
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
    logic settle_t1, settle_t2, settle_t3;
    logic reset_n_s1, reset_n_s2;
    logic urst_t1, urst_t2, urst_t3;
    logic [7:0] urst_cnt;
    logic settled;
    logic run_q;
    logic [3:0] post;
    always_ff @(posedge clk_sys or negedge rst_n) begin
        if (!rst_n) begin
            settle_t1 <= 1'b0;
            settle_t2 <= 1'b0;
            settle_t3 <= 1'b0;
            reset_n_s1 <= 1'b0;
            reset_n_s2 <= 1'b0;
            urst_t1 <= 1'b0;
            urst_t2 <= 1'b0;
            urst_t3 <= 1'b0;
            urst_cnt <= '0;
            settled <= 1'b0;
            run_q <= 1'b0;
            post <= '0;
            pocket_bridge_slot_len <= '0;
            pocket_bridge_slot_set <= 1'b0;
        end else begin
            settle_t1 <= settle_t;
            settle_t2 <= settle_t1;
            settle_t3 <= settle_t2;
            reset_n_s1 <= reset_n;
            reset_n_s2 <= reset_n_s1;
            urst_t1 <= urst_t;
            urst_t2 <= urst_t1;
            urst_t3 <= urst_t2;
            if (urst_t2 != urst_t3)
                urst_cnt <= 8'hFF;
            else if (urst_cnt != '0)
                urst_cnt <= urst_cnt - 8'd1;
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
    end
    always_comb pocket_bridge_run = reset_n_s2 && settled && sdram_ready
        && urst_cnt == '0;

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
    logic [31:0] pk_s1, pk_s2, pj_s1, pj_s2, kk_s1, kk_s2, kj_s1, kj_s2;
    logic [31:0] mk_s1, mk_s2, mj_s1, mj_s2;
    logic [31:0] up_s1, up_s2, uc_s1, uc_s2, ut_s1, ut_s2;
    logic [31:0] um_s1, um_s2, us_s1, us_s2;
    logic [31:0] re_s1, re_s2;
    logic rv_s1, rv_s2;
    logic [15:0] pt_s1, pt_s2, kt_s1, kt_s2, mt_s1, mt_s2;
    always_ff @(posedge clk_sys or negedge rst_n) begin
        if (!rst_n) begin
            pk_s1 <= '0; pk_s2 <= '0;
            pj_s1 <= '0; pj_s2 <= '0;
            pt_s1 <= '0; pt_s2 <= '0;
            kk_s1 <= '0; kk_s2 <= '0;
            kj_s1 <= '0; kj_s2 <= '0;
            kt_s1 <= '0; kt_s2 <= '0;
            up_s1 <= '0; up_s2 <= '0;
            uc_s1 <= '0; uc_s2 <= '0;
            ut_s1 <= '0; ut_s2 <= '0;
            um_s1 <= '0; um_s2 <= '0;
            us_s1 <= '0; us_s2 <= '0;
            re_s1 <= '0; re_s2 <= '0;
            rv_s1 <= 1'b0; rv_s2 <= 1'b0;
            pocket_bridge_set_phi2 <= '0;
            pocket_bridge_set_cp <= '0;
            pocket_bridge_set_tz <= '0;
            pocket_bridge_set_tz_min <= '0;
            pocket_bridge_set_tz_sign <= '0;
            pocket_bridge_rtc_epoch <= '0;
            pocket_bridge_rtc_valid <= 1'b0;
            mk_s1 <= '0; mk_s2 <= '0;
            mj_s1 <= '0; mj_s2 <= '0;
            mt_s1 <= '0; mt_s2 <= '0;
            pocket_bridge_mou_key <= '0;
            pocket_bridge_mou_joy <= '0;
            pocket_bridge_mou_trig <= '0;
            pocket_bridge_pad_key <= '0;
            pocket_bridge_pad_joy <= '0;
            pocket_bridge_pad_trig <= '0;
            pocket_bridge_kbd_key <= '0;
            pocket_bridge_kbd_joy <= '0;
            pocket_bridge_kbd_trig <= '0;
        end else begin
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
            up_s1 <= uphi2_74; up_s2 <= up_s1;
            uc_s1 <= ucp_74;   uc_s2 <= uc_s1;
            ut_s1 <= utz_74;   ut_s2 <= ut_s1;
            um_s1 <= utzm_74;  um_s2 <= um_s1;
            us_s1 <= utzs_74;  us_s2 <= us_s1;
            re_s1 <= rtc_epoch; re_s2 <= re_s1;
            rv_s1 <= rtc_valid; rv_s2 <= rv_s1;
            if (up_s1 == up_s2) pocket_bridge_set_phi2 <= up_s2;
            if (uc_s1 == uc_s2) pocket_bridge_set_cp <= uc_s2;
            if (ut_s1 == ut_s2) pocket_bridge_set_tz <= ut_s2;
            if (um_s1 == um_s2) pocket_bridge_set_tz_min <= um_s2;
            if (us_s1 == us_s2) pocket_bridge_set_tz_sign <= us_s2;
            if (re_s1 == re_s2) pocket_bridge_rtc_epoch <= re_s2;
            pocket_bridge_rtc_valid <= rv_s2;
            if (mk_s1 == mk_s2) pocket_bridge_mou_key <= mk_s2;
            if (mj_s1 == mj_s2) pocket_bridge_mou_joy <= mj_s2;
            if (mt_s1 == mt_s2) pocket_bridge_mou_trig <= mt_s2;
        end
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_pocket_bridge;
    always_comb unused_pocket_bridge = ^{bridge_addr[27:26]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
