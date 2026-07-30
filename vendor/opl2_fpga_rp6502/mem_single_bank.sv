// rp6502: the conditional instantiation below is written as a bare if/else
// at module scope. SystemVerilog reads that as an implicit generate and
// Vivado accepts it; Quartus does not, and stops at the `if`. Wrapped in
// generate/endgenerate, which changes nothing about what is built.
/*******************************************************************************
#   +html+<pre>
#
#   FILENAME: mem_single_bank.sv
#   AUTHOR: Greg Taylor     CREATION DATE: 20 Sept 2025
#
#   DESCRIPTION:
#
#   CHANGE HISTORY:
#   20 Sept 2025    Greg Taylor
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
#   Copyright (C) 2012  Steffen Ohrendorf <steffen.ohrendorf@gmx.de>
#
#   Some code based on forum posts in:
#   http://forums.submarine.org.uk/phpBB/viewforum.php?f=9,
#   Copyright (C) 2010-2013 by carbon14 and opl3
#
#******************************************************************************/
`timescale 1ns / 1ps
`default_nettype none

module mem_single_bank #(
    parameter DATA_WIDTH = 0,
    parameter DEPTH = 0,
    parameter OUTPUT_DELAY = 0, // 0, 1, or 2
    parameter DEFAULT_VALUE = 0
) (
    input wire clk,
    input wire wea,
    input wire reb, // only used if OUTPUT_DELAY >0
    input wire [$clog2(DEPTH)-1:0] addra,
    input wire [$clog2(DEPTH)-1:0] addrb,
    input wire [DATA_WIDTH-1:0] dia,
    output logic [DATA_WIDTH-1:0] dob
);
generate
    if (OUTPUT_DELAY == 0)
        mem_simple_dual_port_async_read #(
            .DATA_WIDTH(DATA_WIDTH),
            .DEPTH(DEPTH),
            .DEFAULT_VALUE(DEFAULT_VALUE)
        ) mem_bank (
            .clka(clk),
            .wea,
            .addra,
            .addrb,
            .dia,
            .dob
        );
    else
        mem_simple_dual_port #(
            .DATA_WIDTH(DATA_WIDTH),
            .DEPTH(DEPTH),
            .OUTPUT_DELAY(OUTPUT_DELAY),
            .DEFAULT_VALUE(DEFAULT_VALUE)
        ) mem_bank (
            .clka(clk),
            .clkb(clk),
            .wea,
            .reb,
            .addra,
            .addrb,
            .dia,
            .dob
        );
endgenerate
endmodule
`default_nettype wire
