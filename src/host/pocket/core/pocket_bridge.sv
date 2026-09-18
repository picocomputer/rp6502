/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * core_top ties bridge_endian_little to 0, so the first byte of each
 * slot word arrives in bridge_wr_data[31:24]. Each word is written to
 * the SDRAM as two halfwords with the even byte in the low half.
 */

module pocket_bridge (
    input logic clk_74a,
    input logic arst_n,
    input logic bridge_wr,
    input logic [31:0] bridge_addr,
    input logic [31:0] bridge_wr_data,
    input logic dataslot_allcomplete,
    input logic dataslot_update,
    input logic reset_n,
    output logic [9:0] pocket_bridge_dt_addr,
    output logic pocket_bridge_dt_busy,
    input logic [31:0] datatable_q,
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
    output logic [7:0] pocket_bridge_upd_n,
    output logic [3:0][31:0] pocket_bridge_cont_key,
    output logic [3:0][31:0] pocket_bridge_cont_joy,
    output logic [3:0][15:0] pocket_bridge_cont_trig,
    output logic [31:0] pocket_bridge_set_tz,
    output logic [31:0] pocket_bridge_set_tz_min,
    output logic [31:0] pocket_bridge_set_tz_sign,
    output logic [31:0] pocket_bridge_set_kb,

    input logic [31:0] rtc_epoch,
    input logic rtc_valid,
    output logic [31:0] pocket_bridge_rtc_epoch,
    output logic pocket_bridge_rtc_valid
);

    logic pend_second;
    logic [24:0] pend_addr;
    logic [15:0] pend_data;
    logic wf_full, wf_empty;
    logic wf_stb;
    logic [40:0] wf_wdata;
    logic slot_wr;
    always_comb slot_wr = bridge_wr && bridge_addr[31:28] == 4'h0;

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
            /* The Gray value is registered on clk_74a so that only one
             * bit changes per increment at the synchroniser's input, and
             * a sample taken during an increment holds either the old
             * count or the new one. If the conversion were done on
             * clk_sys, the synchroniser would sample the binary counter
             * instead. */
            upd_g_74 <= b2g(upd_n + 8'd1);
        end
    end

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

    logic [31:0] utz_74, utzm_74, utzs_74;
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

    /* The APF bridge has no wait signal, and w_stb is gated on
     * !wf_full, so a write that arrives while the queue is full is
     * dropped. Analogue's io_bridge_peripheral.v gives the fastest host
     * access as one every 88 clk_74a cycles, about 1185 ns. Each word is
     * two entries, so eight entries hold about 4.7 microseconds of
     * writes at that rate. */
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

    /* slot_size crosses to clk_sys without a synchroniser. Outside
     * reset it changes only on the clk_74a edge that toggles settle_t,
     * and it is sampled on clk_sys only after that toggle has passed
     * through two flops, so it is stable when sampled. */
    logic reset_n_q;
    logic allcomplete_q;
    logic [1:0] dt_read;
    logic [31:0] slot_size;
    logic settle_t;
    /* Port A of the data table is shared with pocket_file. This read is
     * started by an edge and cannot wait, so pocket_bridge_dt_busy keeps
     * pocket_file off the port from the trigger until the word is
     * captured.
     *
     * Word 1 is the size in the first table entry, which holds the ROM
     * slot. dataslot_allcomplete is set by host command 0x008F and
     * cleared only by the next slot request. A hot reload of the ROM was
     * measured as a slot request write, the new image and its table
     * entry, and a second 0x008F. No 0x008A arrives, because the host
     * sends 0x008A only for deferload slots and the ROM slot is not one.
     * The new size is therefore read and posted to the running machine
     * after the rising edge of dataslot_allcomplete. */
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
        if ((pocket_bridge_run && !run_q)
            || (settle_t2 != settle_t3 && pocket_bridge_run))
            post[0] <= 1'b1;
        if (post[3])
            pocket_bridge_slot_set <= 1'b1;
    end
    always_comb pocket_bridge_run = reset_n_s2 && settled && sdram_ready;

    always_comb pocket_bridge_upd_n = upd_n_sys;

    /* Each multi-bit word is taken only when two consecutive samples
     * agree, so a sample caught while the word was changing is never
     * passed on. */
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
