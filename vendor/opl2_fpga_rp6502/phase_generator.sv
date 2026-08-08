// rp6502: the two operator tables read from one shared ROM.
//
// The only delta from the vendor file is below at the LUT instances:
// opl2_log_sine_lut and opl2_exp_lut become one opl2_lut_rom with the
// same two registered reads, so both tables live in one M10K instead
// of half-filling two. Everything else is the vendor's, verbatim.

/*******************************************************************************
#   +html+<pre>
#
#   FILENAME: phase_generator.sv
#   AUTHOR: Greg Taylor     CREATION DATE: 13 Oct 2014
#
#   DESCRIPTION:
#
#   CHANGE HISTORY:
#   3 June 2024     Greg Taylor
#       Refactored to match implementation in https://github.com/nukeykt/Nuked-OPL3
#
#   13 Oct 2014    Greg Taylor
#       Initial version
#
#   Copyright (C) 2014 Greg Taylor <gtaylor@sonic.net>
#
#   This file is part of OPL3 FPGA.
#
#   OPL3 FPGA is free software: you can redistribute it and/or modify
#   it under the terms of the GNU Lesser General Public License as published by
#   the Free Software Foundation, either version 3 of the License, or
#   (at your option) any later version.
#
#   OPL3 FPGA is distributed in the hope that it will be useful,
#   but WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#   GNU Lesser General Public License for more details.
#
#   You should have received a copy of the GNU Lesser General Public License
#   along with OPL3 FPGA.  If not, see <http://www.gnu.org/licenses/>.
#
#   Original Java Code:
#   Copyright (C) 2008 Robson Cozendey <robson@cozendey.com>
#
#   Original C++ Code:
#   Copyright (C) 2013-2020 Nuke.YKT
#
#   Some code based on forum posts in:
#   http://forums.submarine.org.uk/phpBB/viewforum.php?f=9,
#   Copyright (C) 2010-2013 by carbon14 and opl3
#
#******************************************************************************/
`timescale 1ns / 1ps
`default_nettype none

module phase_generator
    import opl2_pkg::*;
(
    input wire clk,
    input wire sample_clk_en,
    input wire [OP_NUM_WIDTH-1:0] op_num,
    input wire [PHASE_ACC_WIDTH-1:0] phase_inc_p2,
    input wire [REG_WS_WIDTH-1:0] ws,
    input wire [FINAL_ENV_WIDTH-1:0] env_p3,
    input wire pg_reset_p2,
    input wire [OP_OUT_WIDTH-1:0] modulation_p1,
    input var operator_t op_type_p0,
    output logic signed [OP_OUT_WIDTH-1:0] out_p6 = 0
);
    localparam LOG_SIN_OUT_WIDTH = 12;
    localparam EXP_OUT_WIDTH = 10;
    localparam PIPELINE_DELAY = 6;

    logic [PIPELINE_DELAY:1] sample_clk_en_p;
    logic [PHASE_ACC_WIDTH-1:0] phase_acc_p2;
    logic [PHASE_FINAL_WIDTH-1:0] phase_acc_shifted_p2;
    logic [PHASE_ACC_WIDTH-1:0] phase_acc_p3 = 0;
    logic [PHASE_FINAL_WIDTH-1:0] final_phase_p3;
    logic [7:0] theta_p3;
    logic [PHASE_FINAL_WIDTH-1:0] rhythm_phase_p3;
    logic [LOG_SIN_OUT_WIDTH-1:0] log_sin_out_p4;
    logic [OP_OUT_WIDTH-1:0] pre_gain_p4;
    logic [FINAL_ENV_WIDTH+4-1:0] post_gain_p4; // need extra bits to detect overflow
    logic [PIPELINE_DELAY:1] [REG_WS_WIDTH-1:0] ws_p;
    logic [PIPELINE_DELAY:1] [OP_NUM_WIDTH-1:0] op_num_p;
    logic [PIPELINE_DELAY:1] [$bits(operator_t)-1:0] op_type_p;
    logic [PIPELINE_DELAY:2] [OP_OUT_WIDTH-1:0] modulation_p;
    logic [PIPELINE_DELAY:4] [PHASE_FINAL_WIDTH-1:0] final_phase_p;
    logic [FINAL_ENV_WIDTH+3-1:0] env_shifted_p4 = 0;
    logic [OP_OUT_WIDTH-1:0] level_p4;
    logic [OP_OUT_WIDTH-1:0] level_p5 = 0;
    logic [EXP_OUT_WIDTH-1:0] exp_out_p5;
    logic [OP_OUT_WIDTH-1:0] exp_out_post_level_p5;
    logic [OP_OUT_WIDTH-1:0] neg_p5 = 0;

    pipeline_sr #(
        .ENDING_CYCLE(PIPELINE_DELAY)
    ) sample_clk_en_sr (
        .clk,
        .in(sample_clk_en),
        .out(sample_clk_en_p)
    );

    pipeline_sr #(
        .DATA_WIDTH(OP_NUM_WIDTH),
        .ENDING_CYCLE(PIPELINE_DELAY)
    ) op_num_sr (
        .clk,
        .in(op_num),
        .out(op_num_p)
    );

    pipeline_sr #(
        .DATA_WIDTH($bits(operator_t)),
        .ENDING_CYCLE(PIPELINE_DELAY)
    ) op_type_sr (
        .clk,
        .in(op_type_p0),
        .out(op_type_p)
    );

    pipeline_sr #(
        .DATA_WIDTH(OP_OUT_WIDTH),
        .STARTING_CYCLE(2),
        .ENDING_CYCLE(PIPELINE_DELAY)
    ) modulation_sr (
        .clk,
        .in(modulation_p1),
        .out(modulation_p)
    );

    pipeline_sr #(
        .DATA_WIDTH(PHASE_FINAL_WIDTH),
        .STARTING_CYCLE(4),
        .ENDING_CYCLE(PIPELINE_DELAY)
    ) final_phase_sr (
        .clk,
        .in(final_phase_p3),
        .out(final_phase_p)
    );

    pipeline_sr #(
        .DATA_WIDTH(REG_WS_WIDTH),
        .ENDING_CYCLE(PIPELINE_DELAY)
    ) ws_post_opl_sr (
        .clk,
        .in(ws),
        .out(ws_p)
    );

    mem_single_bank #(
        .DATA_WIDTH(PHASE_ACC_WIDTH),
        .DEPTH(NUM_OPERATORS_PER_BANK),
        .OUTPUT_DELAY(2),
        .DEFAULT_VALUE(0)
    ) phase_acc_mem (
        .clk,
        .wea(sample_clk_en_p[3]),
        .reb(sample_clk_en),
        .addra(op_num_p[3]),
        .addrb(op_num),
        .dia(phase_acc_p3),
        .dob(phase_acc_p2)
    );

    /*
     * Phase Accumulator. Modulation and rhythm get added to the final phase but not
     * back into the accumulator.
     */
    always_ff @(posedge clk)
        if (pg_reset_p2)
            phase_acc_p3 <= 0;
        else
            phase_acc_p3 <= phase_acc_p2 + phase_inc_p2;

    always_comb phase_acc_shifted_p2 = phase_acc_p2 >> 9;

    /*
     * Some rhythm instruments modify the phase, otherwise pass-through normally.
     * Bottom bits of phase accumulator are fractional and get dropped off.
     */
     calc_rhythm_phase calc_rhythm_phase (
        .phase_p2(phase_acc_shifted_p2),
        .*
    );

    always_comb begin
        final_phase_p3 = rhythm_phase_p3 + modulation_p[3];

        unique case (ws_p[3])
        0, 1, 2: theta_p3 = final_phase_p3[8] ? final_phase_p3[7:0] ^ 'hff : final_phase_p3[7:0];
        3:       theta_p3 = final_phase_p3[7:0];
        endcase
    end

    opl2_lut_rom lut_rom_inst (
        .theta(theta_p3),
        .log_sin_out(log_sin_out_p4),
        .exp_in(~post_gain_p4[7:0]),
        .exp_out(exp_out_p5),
        .*
    );

    always_ff @(posedge clk)
        env_shifted_p4 <= env_p3 << 3;

    always_comb begin
        unique case (ws_p[4])
        0, 2:       pre_gain_p4 = log_sin_out_p4;
        1:          pre_gain_p4 = final_phase_p[4][9] ? 'h1000 : log_sin_out_p4; // setting msb effectively mutes
        3:          pre_gain_p4 = final_phase_p[4][8] ? 'h1000 : log_sin_out_p4;
        endcase

        post_gain_p4 = pre_gain_p4 + env_shifted_p4;
        level_p4 = post_gain_p4 > 'h1fff ? 'h1fff : post_gain_p4; // clamp level
    end

    always_ff @(posedge clk) begin
        level_p5 <= level_p4;

        neg_p5 <= 0;
        if (ws_p[4] == 0)
            neg_p5 <= final_phase_p[4][9] ? '1 : 0;
    end

    always_comb
        exp_out_post_level_p5 = ((exp_out_p5 | 'h400) << 1) >> (level_p5 >> 8);

    always_ff @(posedge clk)
        out_p6 <= exp_out_post_level_p5 ^ neg_p5;

endmodule
`default_nettype wire

