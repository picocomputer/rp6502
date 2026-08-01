/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The bell of ria/aud/bel.c, reduced to the one sound this machine can
 * ring: bel_teletype, struck by the console's BEL scan. Every derived
 * rate in the C is a function of the preset and the sample rate, so the
 * divider, the tables, and the millisecond thresholds all collapse to
 * constants — the triangle's grit window, the 8 ms attack to -5, the
 * 6 ms decay to -6, the 750 ms release, restrike at 100 ms, release at
 * 20 ms, end at 800 ms.
 *
 * RATE is a parameter because there are two instances at two rates.
 * They were both frozen at 24 kHz, which was right inside aud_psg and
 * wrong inside aud_opl: stepped once per OPL sample at 49,704 Hz, the
 * bell rang at 3,645 Hz instead of 1,760 with its whole envelope
 * compressed 2.07x. It is stated at the instance now so it cannot
 * silently drift again. The queue of eight becomes an occupancy
 * counter, since every entry is the same sound. One step per sample
 * walks the C's exact order: elapsed time first, restrike skipping the
 * release and end checks, generate from the advanced phase, envelope,
 * then the output under the fresh volume.
 */

module aud_bel #(
    /* Samples per second this instance is stepped at. */
    parameter int RATE = 24000
) (
    input logic clk,
    input logic rst_n,

    /* bel_add(&bel_teletype): drop when the ring is full. */
    input logic strike,

    /* One bel_sample(24000): advance and register the output. */
    input logic step,

    output logic signed [15:0] aud_bel_out
);

    /* bel.c's arithmetic exactly, including where it truncates: the
     * envelope rates divide by rate/1000 the way bel_env_rate does. */
    localparam longint unsigned KHZ = 64'(RATE) / 64'd1000;
    localparam logic [31:0] PHASE_INC =
        32'((64'd4294967296 * 64'd1760) / 64'd3 / 64'(RATE));  /* 1760 Hz */
    localparam logic [7:0] DUTY_HALF = 8'd107;          /* 215 >> 1 */
    localparam logic [31:0] ATK_RATE = 32'((1 << 24) / (KHZ * 8));
    localparam logic [31:0] ATK_TARGET = 32'd102 << 16; /* -5 vol */
    localparam logic [31:0] DEC_RATE = 32'((1 << 24) / (KHZ * 6));
    localparam logic [31:0] DEC_TARGET = 32'd86 << 16;  /* -6 vol */
    localparam logic [31:0] REL_RATE = 32'((1 << 24) / (KHZ * 750));
    /* Sixteen bits, not fifteen: 800 ms at the OPL's rate is 39,200 and
     * the old width stopped at 32,767, so a rate-parameterised bell
     * would have wrapped its end-of-life counter. */
    localparam logic [15:0] RESTRIKE_AT = 16'(KHZ * 100);
    localparam logic [15:0] RELEASE_AT = 16'(KHZ * 20);
    localparam logic [15:0] END_AT = 16'(KHZ * 800);

    localparam logic [1:0] ADSR_RELEASE = 2'd0;
    localparam logic [1:0] ADSR_ATTACK = 2'd1;
    localparam logic [1:0] ADSR_DECAY = 2'd2;
    localparam logic [1:0] ADSR_SUSTAIN = 2'd3;

    logic active;
    logic [2:0] count;
    logic [1:0] adsr;
    logic [31:0] vol;
    logic [31:0] phase;
    logic [15:0] elapsed;

    logic n_active;
    logic [2:0] n_count;
    logic [1:0] n_adsr;
    logic [31:0] n_vol;
    logic [31:0] n_phase;
    logic [15:0] n_elapsed;
    logic signed [15:0] n_out;

    logic gen;
    logic [31:0] phase_n;
    logic [7:0] ph;
    logic signed [15:0] wave;

    always_comb begin
        n_active = active;
        n_count = count;
        n_adsr = adsr;
        n_vol = vol;
        n_phase = phase;
        n_elapsed = elapsed;
        n_out = aud_bel_out;
        gen = 1'b0;
        phase_n = phase;
        ph = '0;
        wave = '0;

        if (step) begin
            if (!active)
                n_out = '0;
            else begin
                gen = 1'b1;
                n_elapsed = elapsed + 16'd1;
                if (n_elapsed >= RESTRIKE_AT && count >= 3'd2) begin
                    n_count = count - 3'd1;
                    n_adsr = ADSR_ATTACK;
                    n_vol = '0;
                    n_elapsed = '0;
                end else begin
                    if (n_elapsed >= RELEASE_AT && adsr != ADSR_RELEASE)
                        n_adsr = ADSR_RELEASE;
                    /* The C's end-with-queue advance is unreachable
                     * here: with a second sound queued, restrike at
                     * 2,400 always fires before end at 19,200. */
                    if (n_elapsed >= END_AT) begin
                        n_count = count - 3'd1;
                        n_active = 1'b0;
                        n_out = '0;
                        gen = 1'b0;
                    end
                end
            end

            if (gen) begin
                phase_n = n_phase + PHASE_INC;
                n_phase = phase_n;
                ph = phase_n[31:24];

                /* The teletype's triangle, duty 215. Sixteen bits, taking
                 * eight more of the same accumulator: the identical ramp
                 * with a finer staircase, wrap included. */
                if (ph < 8'd128 - DUTY_HALF || ph >= 8'd128 + DUTY_HALF)
                    wave = -16'sd32767;
                else if (ph >= 8'd128)
                    wave = 16'(16'sd32767 - $signed(phase_n[30:15]));
                else
                    wave = 16'($signed(phase_n[30:15]) - 16'sd32768);

                case (n_adsr)
                    ADSR_ATTACK: begin
                        n_vol = n_vol + ATK_RATE;
                        if (n_vol >= ATK_TARGET) begin
                            n_vol = ATK_TARGET;
                            n_adsr = ADSR_DECAY;
                        end
                    end
                    ADSR_DECAY: begin
                        if (n_vol <= DEC_RATE)
                            n_vol = '0;
                        else
                            n_vol = n_vol - DEC_RATE;
                        if (n_vol <= DEC_TARGET) begin
                            n_adsr = ADSR_SUSTAIN;
                            n_vol = DEC_TARGET;
                        end
                    end
                    ADSR_SUSTAIN: n_vol = DEC_TARGET;
                    default: begin  /* release */
                        if (n_vol <= REL_RATE)
                            n_vol = '0;
                        else
                            n_vol = n_vol - REL_RATE;
                    end
                endcase

                /* Thirteen bits of envelope, rounded once. It used to
                 * truncate to eight and shift the gap back in as zeros. */
                n_out = 16'((30'($signed(wave) * $signed({1'b0, n_vol[24:12]}))
                    + 30'sd2048) >>> 12);
            end
        end

        /* bel_add lands after the sample boundary, the C's task order. */
        if (strike && n_count != 3'd7) begin
            n_count = n_count + 3'd1;
            if (!n_active) begin
                n_active = 1'b1;
                n_adsr = ADSR_ATTACK;
                n_vol = '0;
                n_phase = '0;
                n_elapsed = '0;
            end
        end
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            active <= 1'b0;
            count <= '0;
            adsr <= ADSR_RELEASE;
            vol <= '0;
            phase <= '0;
            elapsed <= '0;
            aud_bel_out <= '0;
        end else begin
            active <= n_active;
            count <= n_count;
            adsr <= n_adsr;
            vol <= n_vol;
            phase <= n_phase;
            elapsed <= n_elapsed;
            aud_bel_out <= n_out;
        end
    end

endmodule
