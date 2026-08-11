/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

module ria_regs (
    input logic clk,
    input logic clk_mem,
    input logic en,

    input logic cs,
    input logic we,
    input logic [4:0] rs,
    input logic [7:0] data_i,

    output logic [7:0] ria_regs_data,

    output logic [7:0] ria_regs_tx_data,
    output logic ria_regs_tx_valid,

    input logic rx_valid,
    input logic [7:0] rx_data,
    output logic ria_regs_rx_taken,

    input logic vsync_pulse,
    output logic ria_regs_irq,

    output logic ria_regs_xr_busy,
    output logic ria_regs_xr_we,
    output logic [15:0] ria_regs_xr_addr,
    output logic [7:0] ria_regs_xr_wdata,
    input logic [7:0] xr_rdata,
    input logic xr_cpu_want,

    input logic b_we,
    input logic b_re,
    input logic [7:0] b_word,
    input logic [3:0] b_wstrb,
    input logic [31:0] b_wdata,

    input logic sst_own,
    input logic [7:0] sst_word,
    input logic sst_we,
    input logic [31:0] sst_wdata,
    input logic sst_jam,
    input logic [31:0] sst_jam_data[12],
    output logic [31:0] ria_regs_b_rdata,

    output logic ria_regs_api_pending,
    input logic api_ack
);

    localparam logic [7:0] TX_READY = 8'h80;
    localparam logic [7:0] RX_READY = 8'h40;

    logic [7:0] regs[32] ;

    logic [7:0] irq_pending ;
    logic [7:0] irq_enabled ;
    logic [7:0] pend_next;
    always_comb begin
        pend_next = irq_pending;
        if (en && cs && !we && rs == 5'h10)
            pend_next = 8'h00;
        if (en && cs && we && rs == 5'h10)
            pend_next = pend_next & ~data_i;
        if (vsync_pulse)
            pend_next = pend_next | 8'h80;
    end
    always_comb ria_regs_irq = (irq_pending & irq_enabled) != 8'h00;

    (* ramstyle = "MLAB, no_rw_check" *)
    logic [7:0] txf[16];
    logic [3:0] txf_w, txf_r;
    logic [4:0] txf_count;
    logic txf_pop;
    logic push_now;
    always_comb push_now = en && cs && we && rs == 5'h01
        && txf_count < 5'd16 && !sst_own;

    logic xr_wr_pend;
    logic [15:0] xr_wr_addr;
    logic [7:0] xr_wr_byte;
    logic xr_fill_pend0, xr_fill_pend1;
    logic xr_bg_alt;
    logic xr_cap0, xr_cap1;

    logic xr_bg_go, xr_issue_f0, xr_issue_f1;
    always_comb begin
        xr_bg_go = !xr_wr_pend && !xr_fill_pend0 && !xr_fill_pend1
            && !xr_cpu_want;
        xr_issue_f0 = !xr_wr_pend && (xr_fill_pend0 || (xr_bg_go && !xr_bg_alt));
        xr_issue_f1 = !xr_wr_pend && !xr_fill_pend0
            && (xr_fill_pend1 || (xr_bg_go && xr_bg_alt));
        ria_regs_xr_busy = xr_wr_pend || xr_fill_pend0 || xr_fill_pend1
            || xr_bg_go;
        ria_regs_xr_we = xr_wr_pend;
        ria_regs_xr_wdata = xr_wr_byte;
        if (xr_wr_pend)
            ria_regs_xr_addr = xr_wr_addr;
        else if (xr_issue_f0)
            ria_regs_xr_addr = {regs[5'h07], regs[5'h06]};
        else
            ria_regs_xr_addr = {regs[5'h0B], regs[5'h0A]};
    end

    (* ramstyle = "MLAB, no_rw_check" *)
    logic [7:0] xs0[128];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [7:0] xs1[128];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [7:0] xs2[128];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [7:0] xs3[128];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [7:0] xm0[128];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [7:0] xm1[128];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [7:0] xm2[128];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [7:0] xm3[128];
    logic [31:0] xs_guard;
    logic [9:0] xsp;
    logic xs_fill;
    logic [9:0] xs_fill_at;

    logic os_rx_valid ;
    logic [7:0] os_rx_data;
    logic rx_req ;
    logic eff_rx_valid;
    logic [7:0] eff_rx_data;
    always_comb begin
        eff_rx_valid = rx_valid || os_rx_valid;
        eff_rx_data = rx_valid ? rx_data : os_rx_data;
    end

    logic pull;
    always_comb begin
        pull = 1'b0;
        if (cs && !we && eff_rx_valid) begin
            if (rs == 5'h00 && (regs[0] & RX_READY) == 8'h00)
                pull = 1'b1;
            if (rs == 5'h02)
                pull = 1'b1;
        end
    end

    always_comb begin
        ria_regs_data = regs[rs];
        if (cs && !we) begin
            case (rs)
                5'h00: ria_regs_data = (regs[0] & ~TX_READY)
                    | (txf_count < 5'd16 ? TX_READY : 8'h00)
                    | (pull ? RX_READY : 8'h00);
                5'h02: ria_regs_data = regs[2];
                default: ;
            endcase
        end
    end

    logic [7:0] w_word;
    logic w_we;
    logic [3:0] w_strb;
    logic [31:0] w_data;
    always_comb begin
        w_word = sst_own ? sst_word : b_word;
        w_we = sst_own ? sst_we : b_we;
        w_strb = sst_own ? 4'b1111 : b_wstrb;
        w_data = sst_own ? sst_wdata : b_wdata;
    end

    logic [7:0] xs_word;
    logic xs_win;
    always_comb begin
        xs_word = w_word - 8'd64;
        xs_win = w_word >= 8'd64 && w_word <= 8'd192;
    end

    logic xs_push;
    logic [8:0] xs_push_at;
    logic [3:0] xs_os_lane, xs_we;
    logic [6:0] xs_waddr0, xs_waddr1, xs_waddr2, xs_waddr3;
    logic [7:0] xs_wdat0, xs_wdat1, xs_wdat2, xs_wdat3;
    always_comb begin
        xs_push = en && cs && we && rs == 5'h0C && xsp != 10'd0
            && !sst_own;
        xs_push_at = 9'(xsp - 10'd1);
        xs_os_lane = {4{w_we && xs_win && !xs_word[7]}} & w_strb;
        xs_we = xs_os_lane | ({3'd0, xs_push} << xs_push_at[1:0]);
        xs_waddr0 = xs_os_lane[0] ? xs_word[6:0] : xs_push_at[8:2];
        xs_waddr1 = xs_os_lane[1] ? xs_word[6:0] : xs_push_at[8:2];
        xs_waddr2 = xs_os_lane[2] ? xs_word[6:0] : xs_push_at[8:2];
        xs_waddr3 = xs_os_lane[3] ? xs_word[6:0] : xs_push_at[8:2];
        xs_wdat0 = xs_os_lane[0] ? w_data[7:0] : data_i;
        xs_wdat1 = xs_os_lane[1] ? w_data[15:8] : data_i;
        xs_wdat2 = xs_os_lane[2] ? w_data[23:16] : data_i;
        xs_wdat3 = xs_os_lane[3] ? w_data[31:24] : data_i;
    end

    logic [7:0] xs_fill_byte;
    always_comb begin
        case (xs_fill_at[1:0])
            2'd0: xs_fill_byte = xm0[xs_fill_at[8:2]];
            2'd1: xs_fill_byte = xm1[xs_fill_at[8:2]];
            2'd2: xs_fill_byte = xm2[xs_fill_at[8:2]];
            default: xs_fill_byte = xm3[xs_fill_at[8:2]];
        endcase
        if (xs_fill_at[9])
            xs_fill_byte = xs_guard[7:0];
    end

    always_ff @(posedge clk_mem) begin
        if (push_now)
            txf[txf_w] <= data_i;
        if (xs_we[0]) begin
            xs0[xs_waddr0] <= xs_wdat0;
            xm0[xs_waddr0] <= xs_wdat0;
        end
        if (xs_we[1]) begin
            xs1[xs_waddr1] <= xs_wdat1;
            xm1[xs_waddr1] <= xs_wdat1;
        end
        if (xs_we[2]) begin
            xs2[xs_waddr2] <= xs_wdat2;
            xm2[xs_waddr2] <= xs_wdat2;
        end
        if (xs_we[3]) begin
            xs3[xs_waddr3] <= xs_wdat3;
            xm3[xs_waddr3] <= xs_wdat3;
        end
        if (w_we && xs_win && xs_word[7]) begin
            if (w_strb[0])
                xs_guard[7:0] <= w_data[7:0];
            if (w_strb[1])
                xs_guard[15:8] <= w_data[15:8];
            if (w_strb[2])
                xs_guard[23:16] <= w_data[23:16];
            if (w_strb[3])
                xs_guard[31:24] <= w_data[31:24];
        end
    end

    always_comb begin
        case (w_word)
            8'd16: ria_regs_b_rdata = {23'd0, txf_count != 5'd0, txf[txf_r]};
            8'd17: ria_regs_b_rdata = {16'd0, irq_pending, irq_enabled};
            8'd18: ria_regs_b_rdata = {16'd0, os_rx_data, 5'd0,
                                       ria_regs_api_pending, rx_req,
                                       !os_rx_valid};
            8'd200: ria_regs_b_rdata = {22'd0, xsp};
            default: begin
                if (xs_win)
                    ria_regs_b_rdata = xs_word[7] ? xs_guard : {
                        xs3[xs_word[6:0]], xs2[xs_word[6:0]],
                        xs1[xs_word[6:0]], xs0[xs_word[6:0]]
                    };
                else
                    ria_regs_b_rdata = {
                        regs[{w_word[2:0], 2'd3}], regs[{w_word[2:0], 2'd2}],
                        regs[{w_word[2:0], 2'd1}], regs[{w_word[2:0], 2'd0}]
                    };
            end
        endcase
    end

    always_comb txf_pop = b_re && b_word == 8'd16 && txf_count != 5'd0;

    initial begin
        for (int i = 0; i < 32; i++)
            regs[i] = 8'h00;
        ria_regs_tx_data = 8'h00;
        ria_regs_tx_valid = 1'b0;
        ria_regs_rx_taken = 1'b0;
        ria_regs_api_pending = 1'b0;
        irq_pending = 8'h00;
        irq_enabled = 8'h00;
    end
    always_ff @(posedge clk) begin
        if (en) begin
            ria_regs_tx_valid <= 1'b0;
            ria_regs_rx_taken <= pull;
            if (api_ack)
                ria_regs_api_pending <= 1'b0;
            if (cs) begin
                if (we) begin
                    case (rs)
                        5'h01: begin
                            ria_regs_tx_data <= data_i;
                            ria_regs_tx_valid <= 1'b1;
                        end
                        5'h04: begin
                            {regs[5'h07], regs[5'h06]} <=
                                {regs[5'h07], regs[5'h06]}
                                + {{8{regs[5'h05][7]}}, regs[5'h05]};
                        end
                        5'h08: begin
                            {regs[5'h0B], regs[5'h0A]} <=
                                {regs[5'h0B], regs[5'h0A]}
                                + {{8{regs[5'h09][7]}}, regs[5'h09]};
                        end
                        5'h0C: begin
                            if (xsp != 10'd0)
                                regs[5'h0C] <= data_i;
                        end
                        5'h0F: begin
                            regs[5'h0F] <= data_i;
                            regs[5'h11] <= 8'h80;
                            regs[5'h12] <= 8'hFE;
                            ria_regs_api_pending <= 1'b1;
                        end
                        5'h10: ;
                        default: regs[rs] <= data_i;
                    endcase
                end else begin
                    case (rs)
                        5'h00: begin
                            if (pull) begin
                                regs[0] <= regs[0] | RX_READY;
                                regs[2] <= eff_rx_data;
                            end
                        end
                        5'h04: begin
                            {regs[5'h07], regs[5'h06]} <=
                                {regs[5'h07], regs[5'h06]}
                                + {{8{regs[5'h05][7]}}, regs[5'h05]};
                        end
                        5'h08: begin
                            {regs[5'h0B], regs[5'h0A]} <=
                                {regs[5'h0B], regs[5'h0A]}
                                + {{8{regs[5'h09][7]}}, regs[5'h09]};
                        end
                        5'h0C: ;
                        5'h02: begin
                            if (pull) begin
                                regs[2] <= eff_rx_data;
                                regs[0] <= regs[0] | RX_READY;
                            end else begin
                                regs[2] <= 8'h00;
                                regs[0] <= regs[0] & ~RX_READY;
                            end
                        end
                        default: ;
                    endcase
                end
            end
        end else begin
            ria_regs_tx_valid <= 1'b0;
            ria_regs_rx_taken <= 1'b0;
            if (api_ack)
                ria_regs_api_pending <= 1'b0;
        end
            if (vsync_pulse)
                regs[5'h03] <= regs[5'h03] + 8'd1;
            if (en && cs && we && rs == 5'h10)
                irq_enabled <= data_i;
            irq_pending <= pend_next;
            regs[5'h10] <= pend_next;
            if (sst_jam) begin
                irq_enabled <= sst_jam_data[8][7:0];
                irq_pending <= sst_jam_data[8][15:8];
                ria_regs_api_pending <= sst_jam_data[9][2];
            end else if (w_we && w_word == 8'd17) begin
                if (w_strb[0])
                    irq_enabled <= w_data[7:0];
                if (w_strb[1])
                    irq_pending <= w_data[15:8];
            end
        if (xr_cap0)
            regs[5'h04] <= xr_rdata;
        if (xr_cap1)
            regs[5'h08] <= xr_rdata;
        if (xs_fill)
            regs[5'h0C] <= xs_fill_byte;
        if (sst_jam)
            for (int i = 0; i < 8; i++) begin
                regs[{i[2:0], 2'd0}] <= sst_jam_data[i][7:0];
                regs[{i[2:0], 2'd1}] <= sst_jam_data[i][15:8];
                regs[{i[2:0], 2'd2}] <= sst_jam_data[i][23:16];
                regs[{i[2:0], 2'd3}] <= sst_jam_data[i][31:24];
            end
        else if (w_we && w_word < 8'd8) begin
            if (w_strb[0])
                regs[{w_word[2:0], 2'd0}] <= w_data[7:0];
            if (w_strb[1])
                regs[{w_word[2:0], 2'd1}] <= w_data[15:8];
            if (w_strb[2])
                regs[{w_word[2:0], 2'd2}] <= w_data[23:16];
            if (w_strb[3])
                regs[{w_word[2:0], 2'd3}] <= w_data[31:24];
        end
    end

    initial begin
        txf_w = 4'd0;
        txf_r = 4'd0;
        txf_count = 5'd0;
        os_rx_valid = 1'b0;
        os_rx_data = 8'h00;
        rx_req = 1'b0;
        xsp = 10'd512;
        xs_fill = 1'b0;
        xs_fill_at = 10'd0;
        xr_wr_pend = 1'b0;
        xr_wr_addr = 16'h0000;
        xr_wr_byte = 8'h00;
        xr_fill_pend0 = 1'b0;
        xr_fill_pend1 = 1'b0;
        xr_bg_alt = 1'b0;
        xr_cap0 = 1'b0;
        xr_cap1 = 1'b0;
    end
    always_ff @(posedge clk) begin
        xr_cap0 <= xr_issue_f0;
        xr_cap1 <= xr_issue_f1;
        if (xr_wr_pend)
            xr_wr_pend <= 1'b0;
        if (xr_issue_f0)
            xr_fill_pend0 <= 1'b0;
        if (xr_issue_f1)
            xr_fill_pend1 <= 1'b0;
        if (xr_bg_go)
            xr_bg_alt <= !xr_bg_alt;
        if (en && cs && rs == 5'h04) begin
            if (we) begin
                xr_wr_pend <= 1'b1;
                xr_wr_addr <= {regs[5'h07], regs[5'h06]};
                xr_wr_byte <= data_i;
            end
            xr_fill_pend0 <= 1'b1;
        end
        if (en && cs && rs == 5'h08) begin
            if (we) begin
                xr_wr_pend <= 1'b1;
                xr_wr_addr <= {regs[5'h0B], regs[5'h0A]};
                xr_wr_byte <= data_i;
            end
            xr_fill_pend1 <= 1'b1;
        end
        xs_fill <= 1'b0;
        if (en && cs && rs == 5'h0C) begin
            if (we) begin
                if (xsp != 10'd0)
                    xsp <= xsp - 10'd1;
            end else begin
                if (xsp != 10'd512) begin
                    xsp <= xsp + 10'd1;
                    xs_fill <= 1'b1;
                    xs_fill_at <= xsp + 10'd1;
                end else begin
                    xs_fill <= 1'b1;
                    xs_fill_at <= xsp;
                end
            end
        end
        if (sst_jam) begin
            xsp <= sst_jam_data[11][9:0];
            xs_fill <= 1'b1;
            xs_fill_at <= sst_jam_data[11][9:0];
        end
        else if (w_we && w_word == 8'd200)
            xsp <= w_data[9:0];
        if (push_now)
            txf_w <= txf_w + 4'd1;
        if (txf_pop)
            txf_r <= txf_r + 4'd1;
        txf_count <= txf_count + {4'd0, push_now} - {4'd0, txf_pop};
        if (en && cs && !we && !pull
            && ((rs == 5'h00 && (regs[0] & RX_READY) == 8'h00)
                || rs == 5'h02))
            rx_req <= 1'b1;
        if (en && pull)
            rx_req <= 1'b0;
        if (sst_jam) begin
            rx_req <= sst_jam_data[9][1];
            os_rx_valid <= !sst_jam_data[9][0];
            os_rx_data <= sst_jam_data[9][15:8];
        end else if (w_we && w_word == 8'd18 && w_data[9])
            rx_req <= 1'b0;
        else if (w_we && w_word == 8'd18 && !os_rx_valid) begin
            os_rx_valid <= 1'b1;
            os_rx_data <= w_data[7:0];
            rx_req <= 1'b0;
        end else if (en && pull && !rx_valid) begin
            os_rx_valid <= 1'b0;
        end
    end

endmodule
