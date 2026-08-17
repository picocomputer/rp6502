/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The machine's 48 kHz stereo to the Pocket's codec as I2S, timed the
 * way every shipped core times it: MCLK 12.288 MHz from a fractional
 * accumulator on 74.25, SCLK a quarter of that, LRCK at 48 kHz over 64
 * SCLK, sixteen bits MSB-first per channel with the right sample in the
 * LRCK-high slot. The machine already speaks signed sixteen at 48 kHz,
 * so nothing here converts.
 *
 * There is no elasticity: the FIFO only crosses clocks, and the reader
 * drains it into one register. A sample landing between frame reloads
 * is overwritten and a frame with nothing new repeats — so what feeds
 * this has to be periodic, which the machine's sample tick is.
 */

module pocket_i2s (
    /* The machine's domain, on the machine's clock: the savestate gate
     * stops it, and the sample tick beside these words is a level the
     * machine drives. Behind the gate, a tick frozen high by a stop
     * would push the same sample until the queue was full of it; here
     * the pushes simply stop, and the shifter repeats the last sample
     * the way it does through any other silence. */
    input logic clk_mach,
    input logic signed [15:0] aud_l,
    input logic signed [15:0] aud_r,
    input logic aud_valid,

    /* The Pocket's domain. */
    input logic clk_74a,
    input logic arst_n,
    output logic pocket_i2s_mclk,
    output logic pocket_i2s_dac,
    output logic pocket_i2s_lrck
);

    /* The machine hands over sixteen signed bits and the codec wants
     * sixteen signed bits, so there is nothing to do. This used to take a
     * ten-bit level, subtract its center and shift it up six, which put
     * real signal in the top ten and zeros in the bottom six for the
     * whole life of the core. */
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

    /* The freshest sample stands ready for the next frame reload. */
    logic [31:0] latest;
    always_comb take = !fifo_empty;
    always_ff @(posedge clk_74a or negedge arst_n) begin
        if (!arst_n)
            latest <= '0;
        else if (take)
            latest <= fifo_word;
    end

    /* MCLK by fractional accumulator: 4,096 into 12,375, the
     * reference's 245,760 into 742,500 with the common sixty divided
     * out — the identical toggle sequence, seven bits narrower.
     * Toggles at 24.576 MHz for 12.288 average. */
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

    /* SCLK = MCLK / 4, kept as a phase in this domain. */
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
                /* The falling edge launches the next bit: the sixteen
                 * data bits one SCLK after the LRCK edge — the I2S
                 * delay — and zeros through the dummy region where the
                 * reference leaks its held shifter. */
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
