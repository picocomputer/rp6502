/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Mode 3, the linear bitmap of core/vga/mode/mode3.c: the line described
 * to the shared pixel tail as segments. The shared row mapper places the
 * line in the bitmap and makes the oracle's rejects; the tail fetches,
 * slices, looks up the palette and writes the pixels. This front says
 * where in XRAM the window's run starts. A wrapped bitmap is runs of the
 * bitmap's width back to back; a clipped one is a run with padding around
 * it; a rejected line is one padding segment. Doubled, the window is twice
 * the bitmap each way, and the tail draws each pixel of the run twice.
 */

module mode3 (
    input logic clk,

    /* One line of work: start when the config view is valid; abort_i is
     * the next line's deadline. */
    input logic start,
    input logic abort_i,
    input logic [15:0] attr,
    input logic [111:0] cfgw,

    /* What the shared row mapper needs from this mode, and its view of
     * the line in return. */
    output logic [4:0] mode3_win_wf,
    output logic [4:0] mode3_win_hf,
    output logic [19:0] mode3_sizeof_row,
    output logic mode3_addr,
    output logic [14:0] mode3_data_row,
    output logic mode3_seg_on,
    input logic rm_settle,
    input logic signed [16:0] rm_row,
    input logic signed [16:0] rm_col,
    input logic [16:0] rm_row_base,
    input logic rm_run,
    input logic rm_end,

    output logic [2:0] mode3_bpp,
    output logic mode3_reversed,
    output logic mode3_dbl,
    output logic mode3_seg_imm,
    output logic mode3_seg_half,
    output logic [22:0] mode3_seg_bits
);

    logic signed [15:0] cf_width;
    always_comb cf_width = cfgw[63:48];

    /* Attribute code to depth; 8-10 are the reversed 1/2/4. Bits 4 and 5
     * double across and down. */
    logic reversed, hdbl, vdbl;
    logic [2:0] bpp_log;
    always_comb begin
        reversed = attr[3];
        hdbl = attr[4];
        vdbl = attr[5];
        case (attr[2:0])
            3'd0: bpp_log = 3'd0;
            3'd1: bpp_log = 3'd1;
            3'd2: bpp_log = 3'd2;
            3'd3: bpp_log = 3'd3;
            default: bpp_log = 3'd4;
        endcase
    end

    typedef enum logic [1:0] {
        S3_IDLE, S3_WRAP, S3_ADDR, S3_SEG
    } state_t;
    state_t state;

    logic [19:0] sizeof_row;

    always_comb begin
        mode3_win_wf = 5'd1 << hdbl;
        mode3_win_hf = 5'd1 << vdbl;
        mode3_sizeof_row = sizeof_row;
        mode3_addr = state == S3_ADDR;
        mode3_data_row = 15'(rm_row[15:0] >> vdbl);
        mode3_seg_on = state == S3_SEG;
        mode3_bpp = bpp_log;
        mode3_reversed = reversed;
        mode3_dbl = hdbl;
    end

    /* The run's first pixel of the bitmap, and whether the line starts on
     * its right half. */
    always_comb begin
        mode3_seg_imm = !rm_run;
        mode3_seg_half = rm_run && hdbl && rm_col[0];
        mode3_seg_bits = rm_run
            ? ({6'd0, rm_row_base} << 3)
                + ((23'(rm_col[15:0]) >> hdbl) << bpp_log)
            : 23'd0;
    end

    initial begin
        state = S3_IDLE;
        sizeof_row = '0;
    end
    always_ff @(posedge clk) begin
        if (abort_i) begin
`ifdef VERILATOR
            if (state == S3_WRAP || state == S3_ADDR)
                $fatal(1, "mode3 underrun");
`endif
            state <= S3_IDLE;
        end else if (start) begin
            sizeof_row <= ((20'(cf_width) << bpp_log) + 20'd7) >> 3;
            state <= S3_WRAP;
        end else begin
            case (state)
                S3_IDLE: ;
                S3_WRAP:
                    if (rm_settle)
                        state <= S3_ADDR;
                S3_ADDR:
                    state <= S3_SEG;
                S3_SEG:
                    if (rm_end)
                        state <= S3_IDLE;
                default: state <= S3_IDLE;
            endcase
        end
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_mode3;
    always_comb unused_mode3 = ^{cfgw, attr[15:6], rm_row[16],
                                     rm_col[16]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
