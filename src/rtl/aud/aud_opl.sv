/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

module aud_opl #(
    parameter int SAMPLE_SHIFT = 5
) (
    input logic clk,

    input logic xaddr_we,
    input logic [15:0] xaddr_wdata,

    input logic q_we,
    input logic [15:0] q_addr,
    input logic [7:0] q_val,

    output logic signed [15:0] aud_opl_out,
    output logic aud_opl_valid,
    output logic aud_opl_enabled
);

    logic [7:0] page
    logic enabled
    always_comb aud_opl_enabled = enabled;
    initial begin
        page = 8'd0;
        enabled = 1'b0;
    end
    always_ff @(posedge clk) begin
        if (xaddr_we) begin
            page <= xaddr_wdata[15:8];
            enabled <= xaddr_wdata != 16'hFFFF;
        end
    end

    logic snoop;
    always_comb snoop = q_we && enabled && q_addr[15:8] == page;

    logic [7:0] chip_rst;
    logic core_ic_n;
    always_comb core_ic_n = chip_rst == 8'd0;
    initial chip_rst = 8'd255;
    always_ff @(posedge clk) begin
        if (xaddr_we)
            chip_rst <= 8'd255;
        else if (chip_rst != 8'd0)
            chip_rst <= chip_rst - 8'd1;
    end

    typedef enum logic [2:0] {
        S_IDLE, S_SEL, S_SEL_HI, S_DAT, S_DAT_HI
    } state_t;
    state_t state;
    logic [7:0] hold_reg, hold_val;

    logic cs_n, wr_n, address;
    logic [7:0] din;
    always_comb begin
        address = state == S_DAT || state == S_DAT_HI;
        din = (state == S_DAT || state == S_DAT_HI) ? hold_val : hold_reg;
        wr_n = !(state == S_SEL || state == S_DAT);
        cs_n = state == S_IDLE;
    end

    initial begin
        state = S_IDLE;
        hold_reg = 8'd0;
        hold_val = 8'd0;
    end
    always_ff @(posedge clk) begin
        case (state)
            S_IDLE: if (snoop && core_ic_n) begin
                hold_reg <= q_addr[7:0];
                hold_val <= q_val;
                state <= S_SEL;
            end
            S_SEL: state <= S_SEL_HI;
            S_SEL_HI: state <= S_DAT;
            S_DAT: state <= S_DAT_HI;
            default: state <= S_IDLE;
        endcase
    end

`ifdef VERILATOR
    logic [31:0] aud_opl_dropped ;
    initial aud_opl_dropped = 32'd0;
    always_ff @(posedge clk) begin
        if (snoop && core_ic_n && state != S_IDLE)
            aud_opl_dropped <= aud_opl_dropped + 32'd1;
    end
`endif

    logic signed [23:0] core_sample ;
    logic core_valid;
    opl2 opl2 (
        .clk(clk),
        .clk_host(clk),
        .clk_dac(clk),
        .ic_n(core_ic_n),
        .cs_n(cs_n),
        .rd_n(1'b1),
        .wr_n(wr_n),
        .address(address),
        .din(din),
        .dout(),
        .sample_valid(core_valid),
        .sample(core_sample),
        .led(),
        .irq_n()
    );

    logic signed [24:0] mixed;
    always_comb mixed = enabled ? (25'(core_sample) >>> SAMPLE_SHIFT) : 25'sd0;

    initial begin
        aud_opl_out = '0;
        aud_opl_valid = 1'b0;
    end
    always_ff @(posedge clk) begin
        aud_opl_valid <= core_valid;
        if (core_valid) begin
            if (mixed < -25'sd32768)
                aud_opl_out <= -16'sd32768;
            else if (mixed > 25'sd32767)
                aud_opl_out <= 16'sd32767;
            else
                aud_opl_out <= 16'(mixed);
        end
    end

endmodule
