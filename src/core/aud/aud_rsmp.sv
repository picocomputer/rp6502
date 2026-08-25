/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The OPL2's 49,704 samples a second into the codec's 48,000. A YM3812
 * runs at its own rate or it is not a YM3812, so this exists for that one
 * engine and nothing else.
 *
 * Bit-exact with core/aud/rsmp.c, off the same generated coefficients.
 * That one reads pushed and this one pulled, because there the sink is a
 * buffer; they walk the same phase against the same taps in the same
 * order, so the streams stay equal.
 *
 * 128 rows of 24 taps, row p a delay of p/128 of an input sample, linear
 * between rows. One multiply-accumulate walked twice — the row below the
 * phase, then the row above — which is the same arithmetic as
 * interpolating the coefficients and filtering once, at one coefficient
 * read per tap instead of two. Forty-eight cycles of the thousand
 * between input samples.
 *
 * Only rows 0..64 are stored; the rest are those read backwards, exact
 * because a windowed sinc is even in its argument.
 */

module aud_rsmp
    import rsmp_coef_pkg::*;
#(
    /* The two clock divisors, not two frequencies: 1014 and 1050 off the
     * same 50.4 MHz is exactly 175/169, a phase sequence of 169 values
     * that cannot drift. Rounded Hz slips a sample every seven
     * seconds. */
    parameter int STEP_NUM = 1050,
    parameter int STEP_DEN = 1014
) (
    input logic clk,

    input logic signed [15:0] in_sample,
    input logic in_valid,

    input logic step,

    output logic signed [15:0] aud_rsmp_out,
    output logic aud_rsmp_valid
);

    /* Q32, and above 1.0 whenever the source outruns the sink — which is
     * the case this exists for — so the phase needs a bit above the
     * fraction and another for the add that overshoots it. */
    localparam longint unsigned STEP =
        ((64'(STEP_NUM) << 32) / 64'(STEP_DEN));
    localparam int PH_W = 34;
    localparam logic [PH_W-1:0] ONE = PH_W'(64'd1 << 32);
    localparam logic [PH_W-1:0] TWO = PH_W'(64'd2 << 32);

    localparam int HALF_ROWS = RSMP_PHASES / 2;  /* rows 0..64 are stored */

    /* A circular buffer, not a shift register: 24 taps read one per cycle
     * is 384 flops as a shift register, and a small memory plus a counter
     * either way the fitter builds it. */
    (* ramstyle = "no_rw_check" *)
    logic signed [15:0] hist[32];
    logic [4:0] wptr;
    logic primed;

    (* ramstyle = "no_rw_check" *)
    logic signed [RSMP_COEF_W-1:0] coef[RSMP_HALF_N];
    initial begin
        for (int i = 0; i < RSMP_HALF_N; i++) coef[i] = RSMP_HALF[i];
    end

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

    /* rptr trails wptr by under a sample: two pointers is what pulling
     * costs, and the history is 32 deep against 24 taps. */
    logic [4:0] rptr;
    logic [4:0] hist_addr;
    always_comb hist_addr = rptr - 5'(RSMP_TAPS) + 5'(tap);

    logic signed [RSMP_COEF_W-1:0] coef_q;
    logic signed [15:0] hist_q;
    always_ff @(posedge clk) begin
        coef_q <= coef[coef_addr];
        hist_q <= hist[hist_addr];
    end

    logic mac_en;
    logic mac_pass;
    logic signed [39:0] prod;
    always_comb prod = 40'(coef_q) * 40'(hist_q);

    /* Sixteen bits of the twenty-five available, matching the C: a Q25
     * fraction against a 2^39 difference carries more than is worth
     * carrying, and a sixteenth of a 128th of a sample is already finer
     * than the coefficients are quantised. */
    logic [15:0] frac;
    always_comb frac = phase[24:9];

    /* Signed by construction: the fraction is a magnitude, but one
     * unsigned operand makes the whole product unsigned in SystemVerilog,
     * and the filter would then work on rising signals only. */
    logic signed [16:0] fracs;
    always_comb fracs = $signed({1'b0, frac});

    logic signed [40:0] diff;
    always_comb diff = 41'(acc_b) - 41'(acc_a);

    logic signed [56:0] scaled;
    always_comb scaled = 57'(diff) * 57'(fracs);

    /* A clock between the multiply and the sum: as one clock it was the
     * core's longest path. There are a thousand clocks between outputs
     * and this engine spends fifty. */
    logic signed [40:0] scaled_q;

    logic signed [41:0] mixed;
    always_comb mixed = 42'(acc_a) + 42'(scaled_q);

    /* Round, do not truncate: a floor on every sample is a half-LSB DC
     * offset, which measures above everything else the filter does. */
    logic signed [41:0] rounded;
    always_comb rounded = (mixed + 42'sd65536) >>> RSMP_Q;

    initial begin
        state = R_IDLE;
        phase = '0;
        wptr = '0;
        rptr = '0;
        primed = 1'b0;
        tap = '0;
        pass = 1'b0;
        acc_a = '0;
        acc_b = '0;
        scaled_q = '0;
        mac_en = 1'b0;
        mac_pass = 1'b0;
        aud_rsmp_out = '0;
        aud_rsmp_valid = 1'b0;
    end
    always_ff @(posedge clk) begin
        aud_rsmp_valid <= 1'b0;

        if (mac_en) begin
            if (mac_pass)
                acc_b <= acc_b + prod;
            else
                acc_a <= acc_a + prod;
        end
        mac_en <= 1'b0;

        if (in_valid) begin
            /* A cold filter rings against 23 zeros and clicks at the
             * start of every sound, so the first sample fills it. */
            if (!primed) begin
                for (int i = 0; i < 32; i++) hist[i] <= in_sample;
                rptr <= 5'd1;
                primed <= 1'b1;
            end else begin
                hist[wptr] <= in_sample;
            end
            wptr <= wptr + 5'd1;
        end

        case (state)
            R_IDLE: begin
                /* Never read past what has arrived. A tick held is a
                 * tick the phase does not advance on, so the stream
                 * stays in step rather than slipping. */
                if (step && primed && wptr != rptr) begin
                    acc_a <= '0;
                    acc_b <= '0;
                    tap <= '0;
                    pass <= 1'b0;
                    state <= R_MAC;
                end
            end

            /* The product lands two cycles later; R_DRAIN waits it out. */
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

            R_DRAIN: state <= R_SCALE;

            R_SCALE: begin
                scaled_q <= 41'(scaled >>> 16);
                state <= R_EMIT;
            end

            R_EMIT: begin
                aud_rsmp_out <= rounded < -42'sd32768 ? -16'sd32768
                    : rounded > 42'sd32767 ? 16'sd32767 : 16'(rounded);
                aud_rsmp_valid <= 1'b1;
                /* Consume the whole inputs and keep the fraction: at
                 * this ratio one most times and two the rest. */
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

endmodule
