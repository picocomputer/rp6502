/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The APF bridge met on our side of the fence. Slot words arrive on
 * clk_74a MSB-first — file byte zero rides wr_data[31:24] — and leave
 * as two halfword writes toward the SDRAM, even byte low. The slot
 * length comes from the data table after the host signals completion,
 * and the machine's release is ordered by construction: the run gate
 * rises only with the SDRAM awake and the slot settled, and the
 * length posts through the sideband two clocks after the rise, since
 * the machine's own reset clears the register it lands in. A host
 * re-reset re-posts it the same way. Controller buttons cross whole,
 * then leave as HID key events — dpad to arrows, A to Enter, B to
 * Escape, select to Tab, start to Enter — posted only into an empty
 * mailbox, so nothing is ever lost under a slow poll, and only while
 * the machine runs: across a reset the tracker clears, and a button
 * held through boot delivers itself the moment the firmware can
 * hear it. Completion arrives as a level from the bridge command
 * block — it clears only at the next slot request — so the settle
 * fires on its edge. A write into the 0x1 window — the interact
 * menu's "Reset 6502" action — dips the run gate for a moment: the
 * machine reboots and reloads the ROM still staged in the SDRAM,
 * the monitor-less machine's reset button.
 */

module pocket_bridge (
    /* The bridge's domain. */
    input logic clk_74a,
    input logic arst_n,
    input logic bridge_wr,
    input logic [31:0] bridge_addr,
    input logic [31:0] bridge_wr_data,
    input logic dataslot_allcomplete,
    input logic reset_n,
    output logic [9:0] pocket_bridge_dt_addr,
    input logic [31:0] datatable_q,
    input logic [31:0] cont1_key,

    /* The machine's domain. */
    input logic clk_sys,
    input logic rst_n,
    input logic sdram_ready,
    input logic key_busy,
    input logic w_take,
    output logic pocket_bridge_w_avail,
    output logic [24:0] pocket_bridge_w_addr,
    output logic [15:0] pocket_bridge_w_data,
    output logic pocket_bridge_run,
    output logic pocket_bridge_slot_set,
    output logic [31:0] pocket_bridge_slot_len,
    output logic pocket_bridge_key_set,
    output logic [8:0] pocket_bridge_key_code
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

    /* The interact action, crossed as a toggle. */
    logic urst_t;
    always_ff @(posedge clk_74a or negedge arst_n) begin
        if (!arst_n)
            urst_t <= 1'b0;
        else if (bridge_wr && bridge_addr[31:28] == 4'h1)
            urst_t <= !urst_t;
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
    always_comb pocket_bridge_dt_addr = 10'd1;
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
            if ((dataslot_allcomplete && !allcomplete_q)
                || (reset_n && !reset_n_q))
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

    /* --- Buttons to key events, clk_sys side. ---
     * The bitmap crosses whole (buttons are slow); each mapped edge
     * leaves as one press or release, paced 4,096 clocks apart so the
     * firmware's poll never loses one under another. */
    localparam int KEYS = 12;
    localparam logic [4:0] BTN[KEYS] = '{
        5'd0, 5'd1, 5'd2, 5'd3, 5'd4, 5'd5,
        5'd6, 5'd7, 5'd8, 5'd9, 5'd14, 5'd15
    };
    localparam logic [7:0] HID[KEYS] = '{
        8'h52, 8'h51, 8'h50, 8'h4F, /* up down left right */
        8'h28, 8'h29,               /* a enter, b escape */
        8'h2C, 8'h2A,               /* x space, y backspace */
        8'h4B, 8'h4E,               /* l pgup, r pgdn */
        8'h2B, 8'h28                /* select tab, start enter */
    };

    logic [31:0] keys_s1, keys_s2;
    logic [KEYS-1:0] reported;
    logic [4:0] pace;
    always_ff @(posedge clk_sys or negedge rst_n) begin
        if (!rst_n) begin
            keys_s1 <= '0;
            keys_s2 <= '0;
            reported <= '0;
            pace <= '0;
            pocket_bridge_key_set <= 1'b0;
            pocket_bridge_key_code <= '0;
        end else begin
            keys_s1 <= cont1_key;
            keys_s2 <= keys_s1;
            pocket_bridge_key_set <= 1'b0;
            if (!pocket_bridge_run) begin
                /* No mailbox to hear an edge: the tracker clears, and
                 * whatever is held at release delivers itself then. */
                reported <= '0;
                pace <= 5'h1F;
            end else if (pace != '0) begin
                pace <= pace - 5'd1;
            end else if (!key_busy) begin
                for (int k = 0; k < KEYS; k++) begin
                    if (keys_s2[BTN[k]] != reported[k]) begin
                        reported[k] <= keys_s2[BTN[k]];
                        pocket_bridge_key_code <=
                            {keys_s2[BTN[k]], HID[k]};
                        pocket_bridge_key_set <= 1'b1;
                        pace <= 5'h1F;
                        break;
                    end
                end
            end
        end
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_pocket_bridge;
    always_comb unused_pocket_bridge = ^{bridge_addr[27:26],
                                         bridge_addr[0]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
