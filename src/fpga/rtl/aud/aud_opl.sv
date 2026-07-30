/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The OPL2, wrapped so the machine sees the same shape it sees for the
 * PSG. Inside is gtaylormb/opl2_fpga under LGPL-3.0, credited in the
 * distribution README; nothing of its interface reaches the machine.
 *
 * ria/aud/opl.c makes the device a 256-byte XRAM page that mirrors the
 * chip's register file, so an offset within the page *is* a register
 * number. On real hardware a queue carried those writes to the emulator
 * between interrupts; here the write goes to the chip as it lands, the
 * same way the PSG's did once the queue came out of it.
 *
 * The core's host side is a YM3812 bus: one write selects a register,
 * the next carries its value, and each is edge-detected off cs_n and
 * wr_n. So a register costs four clocks. Only the 6502's RW engine
 * feeds this snoop, and at 8 MHz PHI2 its writes are at least 6.3
 * clocks apart, so the sequencer is never the narrow part. host_if
 * holds a 64-deep FIFO behind it besides.
 */

module aud_opl #(
    /* DAC_OUTPUT_WIDTH is 24, but that is the container and not the
     * range: one channel at full volume measures a peak of 2^18, so the
     * nine of them together reach about 2^21.
     *
     * ria/aud/opl.c takes emu8950's full-scale 16 bits down by
     * (16 - PWM_BITS - 2) and then clamps to nine, which maps full scale
     * to four times the clamp — the machine runs its OPL hot and lets
     * the loud parts square off. Holding that same ratio here puts 2^21
     * at four times 511, and the shift that does it is ten. One channel
     * then lands at half scale, which is where the measurement says a
     * single voice belongs.
     *
     * Twelve was the first guess, from reading the 24 as a range. It put
     * one voice 18 dB down, quiet enough to read as broken next to the
     * PSG. */
    parameter int SAMPLE_SHIFT = 10
) (
    input logic clk,
    input logic rst_n,

    /* The device register: the sw's validated pointer, 0xFFFF off.
     * opl_xreg rejects anything not page-aligned, so only the page
     * survives the trip. */
    input logic xaddr_we,
    input logic [15:0] xaddr_wdata,

    /* The RW engine's write snoop. */
    input logic q_we,
    input logic [15:0] q_addr,
    input logic [7:0] q_val,

    /* The console's BEL scan: one teletype bell per pulse. */
    input logic bel_strike,

    output logic [9:0] aud_opl_l,
    output logic [9:0] aud_opl_r,
    output logic aud_opl_valid,
    /* Which engine the machine is listening to. Programming either
     * device's pointer is what picks it, so the choice lives with the
     * pointer rather than in a register of its own. */
    output logic aud_opl_enabled
);

    logic [7:0] page /*verilator public_flat_rd*/;
    logic enabled /*verilator public_flat_rd*/;
    always_comb aud_opl_enabled = enabled;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            page <= 8'd0;
            enabled <= 1'b0;
        end else if (xaddr_we) begin
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
    always_comb core_ic_n = rst_n && chip_rst == 8'd0;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            chip_rst <= 8'd255;
        else if (xaddr_we)
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

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= S_IDLE;
            hold_reg <= 8'd0;
            hold_val <= 8'd0;
        end else begin
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

    /* The bell rides the sample tick, as it does on the PSG's grid. */
    logic signed [15:0] bel_out;
    aud_bel bel (
        .clk(clk),
        .rst_n(rst_n),
        .strike(bel_strike),
        .step(core_valid),
        .aud_bel_out(bel_out)
    );

    logic signed [24:0] mixed;
    always_comb mixed = (25'(core_sample) >>> SAMPLE_SHIFT) + 25'(bel_out);

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            aud_opl_l <= 10'd512;
            aud_opl_r <= 10'd512;
            aud_opl_valid <= 1'b0;
        end else begin
            aud_opl_valid <= core_valid;
            if (core_valid) begin
                if (mixed < -25'sd512) begin
                    aud_opl_l <= 10'd0;
                    aud_opl_r <= 10'd0;
                end else if (mixed > 25'sd511) begin
                    aud_opl_l <= 10'd1023;
                    aud_opl_r <= 10'd1023;
                end else begin
                    aud_opl_l <= 10'(mixed + 25'sd512);
                    aud_opl_r <= 10'(mixed + 25'sd512);
                end
            end
        end
    end

endmodule
