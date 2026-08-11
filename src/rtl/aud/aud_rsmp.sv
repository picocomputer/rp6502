/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

module aud_rsmp
    import rsmp_coef_pkg::*;
#(
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

    localparam longint unsigned STEP =
        ((64'(STEP_NUM) << 32) / 64'(STEP_DEN));
    localparam int PH_W = 34;
    localparam logic [PH_W-1:0] ONE = PH_W'(64'd1 << 32);
    localparam logic [PH_W-1:0] TWO = PH_W'(64'd2 << 32);

    localparam int HALF_ROWS = RSMP_PHASES / 2;

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
    logic [5:0] tap;
    logic pass;

    logic signed [39:0] acc_a, acc_b;

    logic [7:0] row;
    always_comb row = 8'(phase[31:25]) + 8'(pass);

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

    logic [15:0] frac;
    always_comb frac = phase[24:9];

    logic signed [16:0] fracs;
    always_comb fracs = $signed({1'b0, frac});

    logic signed [40:0] diff;
    always_comb diff = 41'(acc_b) - 41'(acc_a);

    logic signed [56:0] scaled;
    always_comb scaled = 57'(diff) * 57'(fracs);

    logic signed [40:0] scaled_q;

    logic signed [41:0] mixed;
    always_comb mixed = 42'(acc_a) + 42'(scaled_q);

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
                if (step && primed && wptr != rptr) begin
                    acc_a <= '0;
                    acc_b <= '0;
                    tap <= '0;
                    pass <= 1'b0;
                    state <= R_MAC;
                end
            end

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
