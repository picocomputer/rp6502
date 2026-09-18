/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Each sample replaces latest as soon as it comes out of the FIFO, and
 * the shifter loads latest once per LRCK frame, so a second sample
 * within one frame overwrites the first and a frame with no new sample
 * repeats the last one. The samples therefore have to arrive once per
 * frame, at 48 kHz. aud_valid is the machine's psg_tick, which fires
 * every 1050 clocks of 50.4 MHz.
 */

module pocket_i2s (
    input logic clk_mach,
    input logic signed [15:0] aud_l,
    input logic signed [15:0] aud_r,
    input logic aud_valid,

    input logic clk_74a,
    input logic arst_n,
    output logic pocket_i2s_mclk,
    output logic pocket_i2s_dac,
    output logic pocket_i2s_lrck
);

    logic signed [15:0] s_l, s_r;
    always_comb begin
        s_l = aud_l;
        s_r = aud_r;
    end

    logic fifo_empty, fifo_full;
    logic [31:0] fifo_word;
    logic take;
    pocket_fifo #(
        .WIDTH(32),
        .DEPTH_LOG2(2)
    ) fifo (
        .wclk(clk_mach),
        .w_stb(aud_valid && !fifo_full),
        .w_data({s_r, s_l}),
        .pocket_fifo_full(fifo_full),
        .rclk(clk_74a),
        .r_take(take),
        .pocket_fifo_empty(fifo_empty),
        .pocket_fifo_rdata(fifo_word)
    );

    logic [31:0] latest;
    always_comb take = !fifo_empty;
    always_ff @(posedge clk_74a or negedge arst_n) begin
        if (!arst_n)
            latest <= '0;
        else if (take)
            latest <= fifo_word;
    end

    /* 4096 / 12375 is the 245760 / 742500 of Analogue's reference
     * core_top.v with the common factor of 60 divided out, so the toggle
     * sequence is identical and the accumulator is seven bits narrower.
     * MCLK toggles at 24.576 MHz on average, which makes it a 12.288 MHz
     * clock. */
    logic [14:0] mclk_acc;
    always_ff @(posedge clk_74a or negedge arst_n) begin
        if (!arst_n) begin
            mclk_acc <= '0;
            pocket_i2s_mclk <= 1'b0;
        end else if (mclk_acc >= 15'd12375) begin
            mclk_acc <= mclk_acc - 15'd12375 + 15'd4096;
            pocket_i2s_mclk <= !pocket_i2s_mclk;
        end else begin
            mclk_acc <= mclk_acc + 15'd4096;
        end
    end

    logic mclk_q;
    logic [1:0] sclk_div;
    logic sclk;
    always_comb sclk = sclk_div[1];
    logic sclk_q;

    logic [31:0] shifter;
    logic [4:0] bitcnt;
    always_ff @(posedge clk_74a or negedge arst_n) begin
        if (!arst_n) begin
            mclk_q <= 1'b0;
            sclk_div <= '0;
            sclk_q <= 1'b0;
            shifter <= '0;
            bitcnt <= '0;
            pocket_i2s_dac <= 1'b0;
            pocket_i2s_lrck <= 1'b0;
        end else begin
            mclk_q <= pocket_i2s_mclk;
            if (pocket_i2s_mclk && !mclk_q)
                sclk_div <= sclk_div + 2'd1;
            sclk_q <= sclk;
            if (sclk_q && !sclk) begin
                /* The sixteen data bits start one SCLK after the LRCK
                 * edge, as I2S requires, and the other sixteen bits of
                 * each half are zero. */
                pocket_i2s_dac <= bitcnt < 5'd16 ? shifter[31] : 1'b0;
                bitcnt <= bitcnt + 5'd1;
                if (bitcnt == 5'd31) begin
                    pocket_i2s_lrck <= !pocket_i2s_lrck;
                    if (!pocket_i2s_lrck)
                        shifter <= latest;
                end else if (bitcnt < 5'd16) begin
                    shifter <= {shifter[30:0], 1'b0};
                end
            end
        end
    end

endmodule
