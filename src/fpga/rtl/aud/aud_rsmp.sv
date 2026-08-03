/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The OPL2's 49,704 samples a second into the codec's 48,000, and nothing
 * else. Every other voice on this machine is generated at whatever rate the
 * sink asked for; a YM3812 runs at its own rate or it is not a YM3812, so
 * this exists for that one engine.
 *
 * emu/emu/rsmp.c is the same filter written for a different sink, and the
 * two agree sample for sample. Both read the same coefficients out of the
 * package src/gen/rsmp_coef_gen.py writes, so there is one design and two
 * implementations of it rather than two designs.
 *
 * Pulled, not pushed: the mixer takes one sample on the machine's tick,
 * always one, never none and never two. An input only lands in the history;
 * the tick is what moves the phase and produces a sample. rsmp.c is written
 * the other way round because there the sink is a buffer and a push API is
 * what that platform wants. Neither shape is the other's copy: they walk the
 * same phase against the same taps in the same order, so the stream is the
 * same and the two stay equal.
 *
 * Structure: 128 rows of 24 taps, row p a delay of p/128 of an input
 * sample, and a straight line between adjacent rows for everything in
 * between. One multiply-accumulate walked twice — once against the row
 * below the phase, once against the row above — because that is the same
 * number as interpolating the coefficients and filtering once, and it needs
 * one coefficient read per tap instead of two. Forty-eight cycles of the
 * roughly one thousand between input samples.
 *
 * Only rows 0..64 are stored. The rest are those read backwards, which is
 * exact rather than approximate: a windowed sinc is even in its argument.
 * That is worth six M10K — measured, not assumed — and six M10K is the
 * difference between leaving room for the alternate screen buffer and not.
 */

module aud_rsmp
    import rsmp_coef_pkg::*;
#(
    /* Input frames per output frame, as the two clock divisors rather than
     * as two frequencies. The OPL2 takes a sample every 1014 clk_sys and
     * the codec every 1050, both off the same 50.4 MHz, so the ratio is
     * exactly 175/169 and the phase sequence is one of 169 values that can
     * never drift. Rounded Hz — 49704 into 48000 — is close and not equal,
     * and the difference is a slipped sample every seven seconds. */
    parameter int STEP_NUM = 1050,
    parameter int STEP_DEN = 1014
) (
    input logic clk,
    input logic rst_n,

    /* One OPL2 sample. */
    input logic signed [15:0] in_sample,
    input logic in_valid,

    /* The machine's sample tick: take one out. */
    input logic step,

    output logic signed [15:0] aud_rsmp_out,
    output logic aud_rsmp_valid
);

    /* Input frames per output frame, Q32 — rsmp_step's number. Above 1.0
     * whenever the source outruns the sink, which is the case this exists
     * for, so the phase needs a bit above the fraction and another for the
     * add that overshoots it. */
    localparam longint unsigned STEP =
        ((64'(STEP_NUM) << 32) / 64'(STEP_DEN));
    localparam int PH_W = 34;
    localparam logic [PH_W-1:0] ONE = PH_W'(64'd1 << 32);
    localparam logic [PH_W-1:0] TWO = PH_W'(64'd2 << 32);

    localparam int HALF_ROWS = RSMP_PHASES / 2;  /* rows 0..64 are stored */

    /* ---------------------------------------------------------------- */
    /* The history, as a circular buffer rather than a shift register.    */
    /* ---------------------------------------------------------------- */
    /* Twenty-four sixteen-bit taps read one per cycle. As a shift register
     * that is 384 flops; as a small memory it is one MLAB and a counter,
     * and the synthesis project turns shift-register inference off anyway
     * because a block is a poor trade for a short one. */
    (* ramstyle = "MLAB, no_rw_check" *)
    logic signed [15:0] hist[32];
    logic [4:0] wptr;
    logic primed;

    /* ---------------------------------------------------------------- */
    /* The coefficients.                                                  */
    /* ---------------------------------------------------------------- */
    (* ramstyle = "M10K, no_rw_check" *)
    logic signed [RSMP_COEF_W-1:0] coef[RSMP_HALF_N];
    initial begin
        for (int i = 0; i < RSMP_HALF_N; i++) coef[i] = RSMP_HALF[i];
    end

    /* ---------------------------------------------------------------- */
    /* Sequencer                                                          */
    /* ---------------------------------------------------------------- */
    typedef enum logic [2:0] {
        R_IDLE, R_MAC, R_DRAIN, R_SCALE, R_EMIT
    } state_t;
    state_t state;

    logic [PH_W-1:0] phase;
    logic [PH_W-1:0] phase_next;
    always_comb phase_next = phase + PH_W'(STEP);
    logic [5:0] tap;        /* 0..23 within a pass */
    logic pass;             /* 0: row p, 1: row p+1 */

    /* Accumulators. A coefficient reaches 2^17 and a sample 2^15; twenty-
     * four of those products need 38 bits signed. */
    logic signed [39:0] acc_a, acc_b;

    /* The row this pass walks, and the phase's position between rows. */
    logic [7:0] row;
    always_comb row = 8'(phase[31:25]) + 8'(pass);

    /* Rows past the midpoint are the mirrored rows read backwards. */
    logic [7:0] src_row;
    logic [4:0] src_tap;
    always_comb begin
        if (row <= 8'(HALF_ROWS)) begin
            src_row = row;
            src_tap = 5'(tap);
        end else begin
            src_row = 8'(RSMP_PHASES) - row;
            src_tap = 5'(RSMP_TAPS - 1) - 5'(tap);
        end
    end

    logic [10:0] coef_addr;
    always_comb coef_addr = 11'(src_row) * 11'(RSMP_TAPS) + 11'(src_tap);

    /* Tap 0 is the oldest sample. rptr points one past the newest input the
     * phase has consumed, which trails wptr — the newest that has arrived —
     * by under a sample. Two pointers rather than one is what pulling costs,
     * and the history is thirty-two deep against twenty-four taps, so the
     * slack for it is already there. */
    logic [4:0] rptr;
    logic [4:0] hist_addr;
    always_comb hist_addr = rptr - 5'(RSMP_TAPS) + 5'(tap);

    logic signed [RSMP_COEF_W-1:0] coef_q;
    logic signed [15:0] hist_q;
    always_ff @(posedge clk) begin
        coef_q <= coef[coef_addr];
        hist_q <= hist[hist_addr];
    end

    /* One cycle behind the reads, so the product lands on the cycle after
     * the address was presented. */
    logic mac_en;
    logic mac_pass;
    logic signed [39:0] prod;
    always_comb prod = 40'(coef_q) * 40'(hist_q);

    /* ---------------------------------------------------------------- */
    /* The interpolation between the two passes, and the output scale.    */
    /* ---------------------------------------------------------------- */
    /* Sixteen bits of the twenty-five available, matching the C: the
     * difference of two accumulators reaches 2^39 and a Q25 fraction would
     * take the product past what is worth carrying. A sixteenth of a 128th
     * of a sample is far finer than the coefficients are quantised. */
    logic [15:0] frac;
    always_comb frac = phase[24:9];

    /* Signed by construction. The fraction is a magnitude, but an unsigned
     * operand makes the whole product unsigned in SystemVerilog and the
     * difference below is very much signed — which would show up as the
     * filter working perfectly on rising signals only. */
    logic signed [16:0] fracs;
    always_comb fracs = $signed({1'b0, frac});

    logic signed [40:0] diff;
    always_comb diff = 41'(acc_b) - 41'(acc_a);

    logic signed [56:0] scaled;
    always_comb scaled = 57'(diff) * 57'(fracs);

    /* A clock between the multiply and the sum. Everything from the
     * accumulators to the output register used to be one clock — a
     * forty-one bit subtract, a forty-one by seventeen multiply, two wide
     * adds and the clamp — and it was the longest path in the core when
     * the Pocket's own layer went on top of the machine. There are a
     * thousand clocks between outputs and this engine spends fifty. */
    logic signed [40:0] scaled_q;

    logic signed [41:0] mixed;
    always_comb mixed = 42'(acc_a) + 42'(scaled_q);

    /* Round, do not truncate: a floor on every sample is a half-LSB DC
     * offset, which measures above everything else the filter does. */
    logic signed [41:0] rounded;
    always_comb rounded = (mixed + 42'sd65536) >>> RSMP_Q;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= R_IDLE;
            phase <= '0;
            wptr <= '0;
            rptr <= '0;
            primed <= 1'b0;
            tap <= '0;
            pass <= 1'b0;
            acc_a <= '0;
            acc_b <= '0;
            scaled_q <= '0;
            mac_en <= 1'b0;
            mac_pass <= 1'b0;
            aud_rsmp_out <= '0;
            aud_rsmp_valid <= 1'b0;
        end else begin
            aud_rsmp_valid <= 1'b0;

            if (mac_en) begin
                if (mac_pass)
                    acc_b <= acc_b + prod;
                else
                    acc_a <= acc_a + prod;
            end
            mac_en <= 1'b0;

            /* The input only fills the history. It keeps arriving whether or
             * not the engine is programmed, so this never starves. */
            if (in_valid) begin
                /* A cold filter would ring against twenty-three zeros and
                 * click at the start of every sound, so the first sample
                 * fills the whole history. */
                if (!primed) begin
                    for (int i = 0; i < 32; i++) hist[i] <= in_sample;
                    /* One input consumed: the first output reads the taps
                     * ending at it, which is where the C's first push
                     * leaves its history. */
                    rptr <= 5'd1;
                    primed <= 1'b1;
                end else begin
                    hist[wptr] <= in_sample;
                end
                wptr <= wptr + 5'd1;
            end

            case (state)
                R_IDLE: begin
                    /* Never read past what has arrived. The source outruns
                     * the sink, so this only ever holds at startup — and a
                     * tick held is a tick the phase does not advance on, so
                     * the stream stays in step rather than slipping. */
                    if (step && primed && wptr != rptr) begin
                        acc_a <= '0;
                        acc_b <= '0;
                        tap <= '0;
                        pass <= 1'b0;
                        state <= R_MAC;
                    end
                end

                /* One coefficient and one sample per cycle. The product
                 * lands two cycles later, which is what R_DRAIN waits out. */
                R_MAC: begin
                    mac_en <= 1'b1;
                    mac_pass <= pass;
                    if (tap == 6'(RSMP_TAPS - 1)) begin
                        tap <= '0;
                        if (!pass) begin
                            pass <= 1'b1;
                        end else begin
                            pass <= 1'b0;
                            state <= R_DRAIN;
                        end
                    end else begin
                        tap <= tap + 6'd1;
                    end
                end

                /* The last product is still in flight. */
                R_DRAIN: state <= R_SCALE;

                /* The accumulators are final now: interpolate between the
                 * two rows and hold it for the round. */
                R_SCALE: begin
                    scaled_q <= 41'(scaled >>> 16);
                    state <= R_EMIT;
                end

                R_EMIT: begin
                    aud_rsmp_out <= rounded < -42'sd32768 ? -16'sd32768
                        : rounded > 42'sd32767 ? 16'sd32767 : 16'(rounded);
                    aud_rsmp_valid <= 1'b1;
                    /* One output is worth STEP inputs, and STEP is above one
                     * because the source outruns the sink. Consume the whole
                     * ones and keep the fraction; at this ratio it is one
                     * input most times and two the rest. */
                    if (phase_next >= TWO) begin
                        phase <= phase_next - TWO;
                        rptr <= rptr + 5'd2;
                    end else begin
                        phase <= phase_next - ONE;
                        rptr <= rptr + 5'd1;
                    end
                    state <= R_IDLE;
                end

                default: state <= R_IDLE;
            endcase
        end
    end

endmodule
