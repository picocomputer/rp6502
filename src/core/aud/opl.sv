/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The OPL2, wrapped so the machine sees the shape it sees for the PSG.
 * Inside is gtaylormb/opl2_fpga under LGPL-3.0, credited in the
 * distribution README; none of its interface reaches the machine.
 *
 * The device is a 256-byte XRAM page mirroring the chip's register file,
 * so an offset within the page IS a register number.
 *
 * The core's host side is a YM3812 bus — one write selects a register,
 * the next carries its value, each edge-detected — so a register costs
 * four clocks. At 8 MHz PHI2 the 6502's writes are 6.3 clocks apart, so
 * the sequencer is never the narrow part.
 */

module opl #(
    /* DAC_OUTPUT_WIDTH is 24, but that is the container and not the
     * range: dac_prep parks a saturated 16-bit channel sum at
     * DAC_LEFT_SHIFT = 5, so core_sample is channel <<< 5 and its real
     * range is 2^20 — the whole mix, not one voice.
     *
     * Five is unity, so this engine adds no gain. The RTL YM3812 sets
     * the level and opl.c multiplies emu8950 by four to reach it.
     *
     * THE TWO CONSTANTS ARE NOT INDEPENDENT: move opl.c's four or this
     * five alone and the platforms diverge by 12 dB with every test
     * still passing. They clip together at the same 8192 in emu8950
     * units. */
    parameter int SAMPLE_SHIFT = 5
) (
    input logic clk,

    /* Page-aligned or rejected before it reaches here. */
    input logic xaddr_we,
    input logic [15:0] xaddr_wdata,

    input logic q_we,
    input logic [15:0] q_addr,
    input logic [7:0] q_val,

    /* A YM3812 is mono. */
    output logic signed [15:0] opl_out,
    output logic opl_valid,
    /* Programming either pointer picks the engine, so the choice lives
     * with the pointer rather than a register of its own. */
    output logic opl_enabled
);

    logic [7:0] page /*verilator public_flat_rd*/;
    logic enabled /*verilator public_flat_rd*/;
    always_comb opl_enabled = enabled;
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

    /* opl_xreg resets the chip before it hands the page over, so a
     * program never inherits the last one's voices. The register file
     * is memory that a plain reset walks rather than clears, so the
     * pulse has to outlast that walk; the file is 22 deep and this is
     * the round number above it. */
    logic [7:0] chip_rst;
    logic core_ic_n;
    always_comb core_ic_n = chip_rst == 8'd0;
    /* Full at power-on so the core gets its IC, and reloaded by a
     * pointer write, which is how the firmware resets this chip. */
    initial chip_rst = 8'd255;
    always_ff @(posedge clk) begin
        if (xaddr_we)
            chip_rst <= 8'd255;
        else if (chip_rst != 8'd0)
            chip_rst <= chip_rst - 8'd1;
    end

    /* Four clocks a register: select low, select high, data low, data
     * high. The core latches on the rising edge of its write, so the
     * strobe has to fall between the two halves or the second write is
     * never seen as one. */
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

    logic signed [23:0] core_sample /*verilator public_flat_rd*/;
    logic core_valid;
    /* dout, led and irq_n are the chip's, not the machine's: status
     * reads go nowhere, the LEDs were a dev board's, and opl.c never
     * enabled the timers whose interrupt this is. */
    /* verilator lint_off PINCONNECTEMPTY */
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
    /* verilator lint_on PINCONNECTEMPTY */

    /* Zero when the pointer is parked: the core is free-running and
     * holds whatever it was last playing, so without this a stopped
     * program keeps sounding. */
    logic signed [24:0] mixed;
    always_comb mixed = enabled ? (25'(core_sample) >>> SAMPLE_SHIFT) : 25'sd0;

    initial begin
        opl_out = '0;
        opl_valid = 1'b0;
    end
    always_ff @(posedge clk) begin
        opl_valid <= core_valid;
        if (core_valid) begin
            if (mixed < -25'sd32768)
                opl_out <= -16'sd32768;
            else if (mixed > 25'sd32767)
                opl_out <= 16'sd32767;
            else
                opl_out <= 16'(mixed);
        end
    end

endmodule
