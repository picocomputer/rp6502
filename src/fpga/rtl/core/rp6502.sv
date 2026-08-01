/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The RP6502 machine, independent of the FPGA platform hosting it. Platform
 * wrappers under platform/ adapt this to the Analogue Pocket (APF) or MiSTer;
 * the simulation verilates this module directly and drives it from sim/.
 *
 * So far: the 6502 at a divided PHI2, its 64 KB, the VIA at $FFD0, the
 * RIA's bare UART and register cells at $FFE0, and the soft CPU that owns
 * them all — a Hazard3 whose firmware loads programs into the 6502's memory,
 * writes its vectors, and releases its reset, the way the RIA boots the real
 * machine. The memory map and its quirks follow emu/sys/mem.c: every write
 * also lands in the SRAM shadow, and $FF00-$FFCF reads float at the last
 * value the bus carried.
 *
 * The soft CPU's window on the machine, byte-wide unless noted:
 *   0x10000000  the 6502's 64 KB
 *   0x30000000  the 64 KB XRAM
 *   0x20000000  the RIA register cells; +0x40 pops the 6502's TX ring,
 *               +0x100 the xstack bytes, +0x320 the xstack pointer,
 *               +0x48 offers an RX byte toward the $FFE2 latch
 *   0x40000000  control: bit 0 runs the 6502 (its RESB, inverted)
 *   0x40000004  syscall: bit 0 reads pending; any write acknowledges
 *   0x50000000  the terminal cell memory and scanout registers, word-wide;
 *               +0x20000 the scanline program, canvas, and vsync line
 *   0x60000000  staging, read-only: the platform answers with the byte at
 *               rp6502_stage_addr — the APF data slot on the Pocket, the
 *               bridge model in simulation
 *   0x80000000  the platform's own devices, word-wide and read/write. The
 *               Pocket puts its host file bridge here; a platform with
 *               nothing to put there reads zero
 */

module rp6502
    import rp6502_pkg::*;
#(
    /* The soft CPU's firmware, for synthesis. Simulation loads the
     * arrays directly through the testbench and leaves this empty. */
    parameter TCM_INIT_FILE = "",
    /* clk_sys in kHz, which the PHI2 accumulator counts against. */
    parameter int SYS_KHZ = 50400
) (
    input logic clk_sys,
    /* The soft CPU's clock: half clk_sys and rising with it. Made
     * outside, because a divider made here rises after this module's
     * own registers have settled at the same edge, and a master clocked
     * that late reads a ready the machine has not published. */
    input logic clk_rv,
    input logic rst_n,

    /* Console: TX bytes out of $FFE1, RX bytes offered toward $FFE2. */
    output logic [7:0] rp6502_tx_data,
    output logic rp6502_tx_valid,
    input logic rx_valid,
    input logic [7:0] rx_data,
    output logic rp6502_rx_taken,

    /* The soft CPU's own console and the testbench halt. */
    output logic [7:0] rp6502_rv_tx_data,
    output logic rp6502_rv_tx_valid,
    output logic rp6502_rv_halted,
    output logic [31:0] rp6502_rv_exit_code,

    /* Staging window: the address holds from the pending request; a
     * combinational platform answers the byte before the next system
     * clock, a slow one holds stage_stall until its byte stands on
     * stage_rdata — the strobe waits. */
    output logic [27:0] rp6502_stage_addr,
    output logic rp6502_stage_pend,
    input logic stage_stall,
    input logic [7:0] stage_rdata,

    /* The platform window: a word-wide read/write port onto whatever the
     * board has that the machine does not. The strobe is one clock, and
     * the answer is expected the clock after it, as the video window's
     * is. */
    output logic [27:0] rp6502_host_addr,
    output logic rp6502_host_stb,
    output logic rp6502_host_we,
    output logic [31:0] rp6502_host_wdata,
    input logic [31:0] host_rdata,

    /* Platform sidebands into the soft CPU's registers: the staged slot
     * length after a load, key events off the platform inputs. */
    input logic slot_set,
    input logic [31:0] slot_len,
    input logic key_set,
    input logic [8:0] key_code,
    input logic [31:0] pad_key,
    input logic [31:0] pad_joy,
    input logic [15:0] pad_trig,
    input logic [31:0] kbd_key,
    input logic [31:0] kbd_joy,
    input logic [15:0] kbd_trig,
    input logic [31:0] mou_key,
    input logic [31:0] mou_joy,
    input logic [15:0] mou_trig,
    output logic rp6502_key_pending,

    /* The composed picture, aligned with its data enable, and the canvas
     * it presents — vga.h's encoding, latched at vblank. A platform whose
     * scaler wants the native picture (the Pocket's does) uses the canvas
     * to undo the beam's doubling and letterboxing; one that just wants
     * 640x480 ignores it. */
    output logic [15:0] rp6502_vid_pixel,
    output logic rp6502_vid_de,
    output logic [2:0] rp6502_vid_canvas,

    /* One stereo sample per PSG tick, 10-bit PWM levels. */
    output logic signed [15:0] rp6502_aud_l,
    output logic signed [15:0] rp6502_aud_r,
    output logic rp6502_aud_valid,

    output logic [RP6502_SCANLINE_W-1:0] rp6502_scanline,
    output logic rp6502_vid_frame
);

    /* clk_rv is half clk_sys, per the port comment. Anything the soft
     * CPU measures time with counts against this one. */
    localparam int RV_KHZ = SYS_KHZ / 2;

    /* PHI2, fixed until the soft CPU programs it. */
    logic [15:0] phi2_khz;
    logic phi2_en;
    phi2_div #(.SYS_KHZ(SYS_KHZ)) phi2_div (
        .clk(clk_sys),
        .rst_n(rst_n),
        .phi2_khz(phi2_khz),
        .phi2_div_en(phi2_en)
    );

    /* The 6502 and its bus, decoded per the machine's map. */
    logic [15:0] cpu_addr;
    logic [7:0] cpu_dout, cpu_din;
    logic cpu_we;
    logic via_irq;
    // Opcode-fetch marker; the debug tap will want it, nothing does yet.
    logic cpu_sync;
    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_sync;
    /* verilator lint_on UNUSEDSIGNAL */
    always_comb unused_sync = cpu_sync;

    /* The 6502 runs when the OS says so; cpu_run is its inverted RESB. */
    logic cpu_run /*verilator public_flat_rw*/;
    logic cpu_stp;

    cpu65 cpu (
        .clk(clk_sys),
        .rst_n(rst_n && cpu_run),
        .en(phi2_en),
        .data_i(cpu_din),
        .irq_i(via_irq || ria_irq),
        .nmi_i(1'b0),
        .rdy_i(1'b0),
        .res_i(1'b0),
        .cpu65_addr(cpu_addr),
        .cpu65_data(cpu_dout),
        .cpu65_we(cpu_we),
        .cpu65_sync(cpu_sync),
        .cpu65_stp(cpu_stp)
    );

    logic sel_via, sel_ria, sel_open;
    always_comb begin
        sel_via = cpu_addr[15:4] == 12'hFFD;
        sel_ria = cpu_addr[15:5] == 11'b1111_1111_111;
        sel_open = cpu_addr[15:8] == 8'hFF && !sel_via && !sel_ria;
    end

    /* Every write lands in the shadow, whatever else it hits. */
    logic [7:0] sram_rdata;
    sram64k sram (
        .clk(clk_sys),
        .a_addr(cpu_addr),
        .a_wdata(cpu_dout),
        .a_we(cpu_we && phi2_en),
        .a_rdata(sram_rdata),
        .b_addr(bus_addr[15:0]),
        .b_wdata(bus_wbyte),
        .b_we(bus_stb && bus_we && bus_sel_sram),
        .b_rdata(sram_b_rdata)
    );

    logic ria_irq;
    logic [7:0] via_data;
    via via (
        .clk(clk_sys),
        .rst_n(rst_n),
        .en(phi2_en),
        .cs(sel_via),
        .we(cpu_we),
        .rs(cpu_addr[3:0]),
        .data_i(cpu_dout),
        .via_data(via_data),
        .via_irq(via_irq)
    );

    /* The soft CPU and its window on the machine.
     *
     * Its clock is half this one, so every level it drives stands for
     * two machine clocks and every pulse the machine sends can fall
     * between two of its edges. Three signals care, and all three are
     * fixed here rather than in rv_soc, which does not know it is being
     * clocked slowly.
     *
     * The strobe and the console valid are levels the machine must act
     * on exactly once: narrowed to their first machine clock. Going the
     * other way, slot_set and key_set are one machine clock wide and
     * would fall between the soft CPU's edges: held for two, which
     * always spans one, and both carry a value the far side is holding
     * anyway, so arriving twice costs nothing. */
    logic bus_stb_raw, bus_stb_q;
    logic rv_tx_valid_raw, rv_tx_valid_q;
    logic slot_set_q, key_set_q;
    always_ff @(posedge clk_sys or negedge rst_n) begin
        if (!rst_n) begin
            bus_stb_q <= 1'b0;
            rv_tx_valid_q <= 1'b0;
            slot_set_q <= 1'b0;
            key_set_q <= 1'b0;
        end else begin
            bus_stb_q <= bus_stb_raw;
            rv_tx_valid_q <= rv_tx_valid_raw;
            slot_set_q <= slot_set;
            key_set_q <= key_set;
        end
    end
    always_comb rp6502_rv_tx_valid = rv_tx_valid_raw && !rv_tx_valid_q;

    logic bus_stb, bus_we, bus_pend;
    always_comb bus_stb = bus_stb_raw && !bus_stb_q;
    logic [31:0] bus_addr, bus_wdata;
    logic [3:0] bus_wstrb;
    logic [31:0] bus_rdata;

    /* Counted against RV_KHZ, not SYS_KHZ: mtime_acc is clocked by
     * clk_rv. One microsecond is RV_KHZ/1000 clocks, which is 25.2 of
     * them and so not a whole number. Ten per clock wrapping at a
     * hundredth of the rate keeps the fraction exact. Left at the
     * module's 1/1 and the clock runs RV_KHZ/1000 times fast, which
     * nothing catches because nothing in simulation waits on a real
     * second. */
    rv_soc #(
        .MTIME_ADD(10),
        .MTIME_WRAP(RV_KHZ / 100),
        .TCM_INIT_FILE(TCM_INIT_FILE)
    ) rv (
        .clk(clk_rv),
        .rst_n(rst_n),
        .rv_soc_phi2_khz(phi2_khz),
        .rv_soc_tx_data(rp6502_rv_tx_data),
        .rv_soc_tx_valid(rv_tx_valid_raw),
        .rv_soc_halted(rp6502_rv_halted),
        .rv_soc_exit_code(rp6502_rv_exit_code),
        .slot_set(slot_set || slot_set_q),
        .slot_len(slot_len),
        .key_set(key_set || key_set_q),
        .pad_key(pad_key),
        .pad_joy(pad_joy),
        .pad_trig(pad_trig),
        .kbd_key(kbd_key),
        .kbd_joy(kbd_joy),
        .kbd_trig(kbd_trig),
        .mou_key(mou_key),
        .mou_joy(mou_joy),
        .mou_trig(mou_trig),
        .key_code(key_code),
        .rv_soc_key_pending(rp6502_key_pending),
        .bus_rdy(!(bus_sel_xram && xr_busy)
                 && !(bus_sel_stage && stage_stall)),
        .rv_soc_bus_pend(bus_pend),
        .rv_soc_bus_stb(bus_stb_raw),
        .rv_soc_bus_we(bus_we),
        .rv_soc_bus_addr(bus_addr),
        .rv_soc_bus_wdata(bus_wdata),
        .rv_soc_bus_wstrb(bus_wstrb),
        .bus_rdata(bus_rdata)
    );

    /* Byte lane per sb/lb for the byte-wide windows. */
    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_bus;
    always_comb unused_bus = ^{bus_addr[27:16]};
    /* verilator lint_on UNUSEDSIGNAL */
    logic [7:0] bus_wbyte;
    always_comb begin
        bus_wbyte = bus_wdata[7:0];
        if (bus_wstrb[1])
            bus_wbyte = bus_wdata[15:8];
        if (bus_wstrb[2])
            bus_wbyte = bus_wdata[23:16];
        if (bus_wstrb[3])
            bus_wbyte = bus_wdata[31:24];
    end

    logic bus_sel_sram, bus_sel_regs, bus_sel_ctl, bus_sel_stage,
        bus_sel_vid, bus_sel_xram, bus_sel_aud, bus_sel_host;
    always_comb begin
        bus_sel_sram = bus_addr[31:28] == 4'h1;
        bus_sel_xram = bus_addr[31:28] == 4'h3;
        bus_sel_regs = bus_addr[31:28] == 4'h2;
        bus_sel_ctl = bus_addr[31:28] == 4'h4;
        bus_sel_stage = bus_addr[31:28] == 4'h6;
        bus_sel_vid = bus_addr[31:28] == 4'h5;
        bus_sel_aud = bus_addr[31:28] == 4'h7;
        bus_sel_host = bus_addr[31:28] == 4'h8;
    end

    always_comb begin
        rp6502_host_addr = bus_addr[27:0];
        rp6502_host_stb = bus_stb && bus_sel_host;
        rp6502_host_we = bus_we;
        rp6502_host_wdata = bus_wdata;
    end

    /* The pending request shows the address early for a slow platform;
     * the strobe-captured register holds it through the answer cycle. */
    logic [27:0] stage_addr_q;
    always_comb begin
        rp6502_stage_pend = bus_pend && bus_sel_stage;
        rp6502_stage_addr = rp6502_stage_pend ? bus_addr[27:0]
                                              : stage_addr_q;
    end

    logic api_pending;
    logic bus_ctl_api, bus_vid_prog;
    logic [7:0] sram_b_rdata;
    logic [31:0] regs_b_rdata, regs_b_q;
    logic [31:0] vid_b_rdata;
    // Which target answers: 0 sram, 1 regs, 2 control, 3 staging, 4 vid,
    // 5 xram, 6 the platform's own.
    logic [2:0] bus_rsel;
    always_ff @(posedge clk_sys or negedge rst_n) begin
        if (!rst_n) begin
            cpu_run <= 1'b0;
            bus_rsel <= 3'd0;
            bus_ctl_api <= 1'b0;
            bus_vid_prog <= 1'b0;
            stage_addr_q <= '0;
        end else begin
            if (bus_stb) begin
                bus_rsel <= bus_sel_regs ? 3'd1
                    : (bus_sel_ctl ? 3'd2
                    : (bus_sel_stage ? 3'd3
                    : (bus_sel_vid ? 3'd4
                    : (bus_sel_xram ? 3'd5
                    : (bus_sel_host ? 3'd6 : 3'd0)))));
                bus_ctl_api <= bus_addr[2];
                bus_vid_prog <= bus_addr[17];
                stage_addr_q <= bus_addr[27:0];
                /* Captured at the strobe: ring reads advance their pointer
                 * there, so the answer must not be re-derived afterward. */
                regs_b_q <= regs_b_rdata;
            end
            if (bus_stb && bus_we && bus_sel_ctl && !bus_addr[2])
                cpu_run <= bus_wbyte[0];
        end
    end

    /* Reads answer one cycle after the strobe. The register window is a
     * true word; the byte-wide windows put their byte on every lane so the
     * master's own extract picks the addressed one. */
    logic [7:0] bus_rbyte;
    always_comb begin
        case (bus_rsel)
            3'd2: bus_rbyte = bus_ctl_api ? {7'b0, api_pending}
                : {6'b0, cpu_stp, cpu_run};
            3'd3: bus_rbyte = stage_rdata;
            3'd5: bus_rbyte = xram_b_rdata;
            default: bus_rbyte = sram_b_rdata;
        endcase
        bus_rdata = bus_rsel == 3'd1 ? regs_b_q
            : (bus_rsel == 3'd4
               ? (bus_vid_prog ? vid_prog_b_rdata : vid_b_rdata)
               : (bus_rsel == 3'd6 ? host_rdata : {4{bus_rbyte}}));
    end

    logic [7:0] ria_data;
    ria_regs ria (
        .clk(clk_sys),
        .rst_n(rst_n),
        .en(phi2_en),
        .cs(sel_ria),
        .we(cpu_we),
        .rs(cpu_addr[4:0]),
        .data_i(cpu_dout),
        .ria_regs_data(ria_data),
        .ria_regs_tx_data(rp6502_tx_data),
        .ria_regs_tx_valid(rp6502_tx_valid),
        .rx_valid(rx_valid),
        .rx_data(rx_data),
        .ria_regs_rx_taken(rp6502_rx_taken),
        .vsync_pulse(prog_vsync_pulse),
        .ria_regs_irq(ria_irq),
        .ria_regs_xr_busy(xr_busy),
        .ria_regs_xr_we(xr_we),
        .ria_regs_xr_addr(xr_addr),
        .ria_regs_xr_wdata(xr_wdata),
        .xr_rdata(xram_b_rdata),
        .xr_cpu_want(bus_pend && bus_sel_xram),
        .b_we(bus_stb && bus_we && bus_sel_regs),
        .b_re(bus_stb && !bus_we && bus_sel_regs),
        .b_word(bus_addr[9:2]),
        .b_wstrb(bus_wstrb),
        .b_wdata(bus_wdata),
        .ria_regs_b_rdata(regs_b_rdata),
        .ria_regs_api_pending(api_pending),
        .api_ack(bus_stb && bus_we && bus_sel_ctl && bus_addr[2])
    );

    /* The bus keeps its last value across the unmapped window. */
    logic [7:0] bus_hold;
    always_comb begin
        if (sel_ria)
            cpu_din = ria_data;
        else if (sel_via)
            cpu_din = via_data;
        else if (sel_open)
            cpu_din = bus_hold;
        else
            cpu_din = sram_rdata;
    end

    always_ff @(posedge clk_sys or negedge rst_n) begin
        if (!rst_n)
            bus_hold <= 8'h00;
        else if (phi2_en && !cpu_we)
            bus_hold <= cpu_din;
    end

    /* The raster: one clock per pixel in simulation, the pixel domain on
     * hardware. The scanline port is the beam's line. */
    logic [9:0] vid_h /*verilator public_flat_rd*/;
    logic [9:0] vid_v /*verilator public_flat_rd*/;
    logic vid_de /*verilator public_flat_rd*/;
    logic vid_hsync /*verilator public_flat_rd*/;
    logic vid_vsync /*verilator public_flat_rd*/;
    logic vid_line_start, vid_frame_start;
    always_comb rp6502_vid_frame = vid_frame_start;
    logic vid_vsync_pulse;
    logic prog_vsync_pulse;
    logic vid_px_first, vid_px_last;
    vid_timing vid_timing (
        .clk(clk_sys),
        .rst_n(rst_n),
        .vid_timing_h(vid_h),
        .vid_timing_v(vid_v),
        .vid_timing_px_first(vid_px_first),
        .vid_timing_px_last(vid_px_last),
        .vid_timing_de(vid_de),
        .vid_timing_hsync(vid_hsync),
        .vid_timing_vsync(vid_vsync),
        .vid_timing_line_start(vid_line_start),
        .vid_timing_frame_start(vid_frame_start),
        .vid_timing_vsync_pulse(vid_vsync_pulse)
    );
    always_comb rp6502_scanline = vid_v;

    /* The XRAM. Port A rotates among the requesting mode engines; port B
     * is the RW engine's while busy — the soft CPU's strobe waits — and
     * the engine's background refresh yields to the soft CPU in turn. */
    logic [7:0] xram_b_rdata;
    logic xr_busy, xr_we;
    logic [15:0] xr_addr;
    logic [7:0] xr_wdata;
    logic [31:0] xram_a_rdata;
    /* Bit 18 of the video window is the font store, above the terminal
     * at bit 17's clear half and the scanline program at its set one.
     *
     * The font store: the terminal and the three plane engines read a
     * byte each per character cell, so one port is eight times what
     * they ask for. The terminal and the planes never read together —
     * a plane renders only off the console canvas — so the rotor is
     * fairness between the planes more than arbitration. Mod four, so
     * the wrap is free. */
    logic [3:0] mf_req;
    logic [13:0] mf_addr[4];
    logic [1:0] f_rotor, f_sel;
    logic f_any;
    always_comb begin
        f_sel = f_rotor;
        f_any = 1'b0;
        for (int i = 0; i < 4; i++) begin
            logic [1:0] cand;
            cand = f_rotor + 2'(i);
            if (!f_any && mf_req[cand]) begin
                f_sel = cand;
                f_any = 1'b1;
            end
        end
    end
    always_ff @(posedge clk_sys or negedge rst_n) begin
        if (!rst_n)
            f_rotor <= 2'd0;
        else if (f_any)
            f_rotor <= f_sel + 2'd1;
    end

    logic [7:0] font_bits;
    vid_font vid_font (
        .clk(clk_sys),
        .addr(mf_addr[f_sel]),
        .vid_font_bits(font_bits),
        .w_stb(bus_stb && bus_we && bus_sel_vid && bus_addr[18]),
        .w_addr(bus_addr[13:0]),
        .w_data(bus_wdata)
    );

    /* Three fills, the sprite stage, and the PSG share port A; the
     * scan folds mod five in wider bits. */
    logic [4:0] ma_req;
    logic [13:0] ma_addr[5];
    logic [2:0] a_rotor, a_sel;
    logic a_any;
    /* The pick, for every rotor position at once. Scanning from the
     * live rotor made each candidate depend on the one before it — an
     * adder, a wrap and a priority step per requester, five deep, in
     * front of the address mux and of every requester's grant. Solved
     * for all five positions the answer is one function of the five
     * request bits, and the rotor only chooses between the answers,
     * arriving from a register while they settle. Same truth table:
     * the lowest offset with a request still wins, because the loop
     * counts down and the last assignment stands. */
    logic [2:0] sel_at[5];
    logic any_at[5];
    always_comb begin
        for (int r = 0; r < 5; r++) begin
            sel_at[r] = 3'(r);
            any_at[r] = 1'b0;
            for (int i = 4; i >= 0; i--) begin
                if (ma_req[(r + i) % 5]) begin
                    sel_at[r] = 3'((r + i) % 5);
                    any_at[r] = 1'b1;
                end
            end
        end
        a_sel = sel_at[a_rotor];
        a_any = any_at[a_rotor];
    end
    always_ff @(posedge clk_sys or negedge rst_n) begin
        if (!rst_n)
            a_rotor <= 3'd0;
        else if (a_any)
            a_rotor <= a_sel == 3'd4 ? 3'd0 : a_sel + 3'd1;
    end
    xram64k xram (
        .clk(clk_sys),
        .a_addr(ma_addr[a_sel]),
        .xram64k_a_rdata(xram_a_rdata),
        .b_addr(xr_busy ? xr_addr : bus_addr[15:0]),
        .b_wdata(xr_busy ? xr_wdata : bus_wbyte),
        .b_we(xr_busy ? xr_we : (bus_stb && bus_we && bus_sel_xram)),
        .xram64k_b_rdata(xram_b_rdata)
    );

    logic [31:0] vid_prog_b_rdata;
    logic [2:0] vid_canvas;
    always_comb rp6502_vid_canvas = vid_canvas;
    logic vid_console, vid_x_shift, vid_y_shift;
    logic [9:0] vid_y_offset;

    /* The prog read port rotates through the planes; each engine waits
     * for its slot and captures the entry the clock after. */
    logic [1:0] p_rotor;
    logic [8:0] pm_line[3];
    logic [31:0] pm_entry;
    logic [15:0] pm_config;
    always_ff @(posedge clk_sys or negedge rst_n) begin
        if (!rst_n)
            p_rotor <= 2'd0;
        else
            p_rotor <= p_rotor == 2'd2 ? 2'd0 : p_rotor + 2'd1;
    end

    vid_prog vid_prog (
        .clk(clk_sys),
        .rst_n(rst_n),
        .frame_start(vid_frame_start),
        .v(vid_v),
        .px_first(vid_px_first),
        .vid_prog_vsync_pulse(prog_vsync_pulse),
        .h(vid_h),
        .vid_prog_canvas(vid_canvas),
        .vid_prog_console(vid_console),
        .vid_prog_x_shift(vid_x_shift),
        .vid_prog_y_shift(vid_y_shift),
        .vid_prog_y_offset(vid_y_offset),
        .p_line(pm_line[p_rotor]),
        .p_plane(p_rotor),
        .vid_prog_p_entry(pm_entry),
        .vid_prog_p_config(pm_config),
        .s_idx(sp_s_idx),
        .vid_prog_s_data(sp_s_data),
        .sp_overrun(sp_overrun),
        .plane_underrun(plane_underrun),
        .vid_prog_ov_clear(sp_ov_clear),
        .b_stb(bus_stb && bus_sel_vid && !bus_addr[18]
               && bus_addr[17]),
        .b_we(bus_we),
        .b_addr(bus_addr[15:0]),
        .b_wdata(bus_wdata),
        .vid_prog_b_rdata(vid_prog_b_rdata)
    );

    logic [15:0] term_pix;
    vid_term vid_term (
        .clk(clk_sys),
        .rst_n(rst_n),
        .frame_start(vid_frame_start),
        .h(vid_h),
        .v(vid_v),
        .px_last(vid_px_last),
        .line_start(vid_line_start),
        .vid_term_pix(term_pix),
        .vid_term_f_req(mf_req[3]),
        .vid_term_f_addr(mf_addr[3]),
        .f_gnt(f_any && f_sel == 2'd3),
        .f_data(font_bits),
        .b_stb(bus_stb && bus_sel_vid && !bus_addr[18]
               && !bus_addr[17]),
        .b_we(bus_we),
        .b_addr(bus_addr[16:0]),
        .b_wstrb(bus_wstrb),
        .b_wdata(bus_wdata),
        .vid_term_b_rdata(vid_b_rdata)
    );

    logic [15:0] m_pix[3];
    logic [2:0] m_filled;
    logic [2:0] m_busy, m_rnew, m_rfilled, m_underrun;
    logic [12:0] sp_s_idx;
    logic [31:0] sp_s_data;
    logic [1:0] sp_plane;
    logic sp_we, sp_force, sp_ov_clear;
    logic [15:0] sp_overrun;
    /* Lines a plane failed to fill in time, counted beside the sprite
     * stage's lost races and cleared with them. Saturating, because a
     * count that wraps to zero reads as a healthy machine. */
    logic [15:0] plane_underrun;
    always_ff @(posedge clk_sys or negedge rst_n) begin
        if (!rst_n)
            plane_underrun <= '0;
        else if (sp_ov_clear)
            plane_underrun <= '0;
        else if (|m_underrun && plane_underrun != 16'hFFFF)
            plane_underrun <= plane_underrun + 16'd1;
    end
    logic [9:0] sp_addr;
    logic [15:0] sp_data;
    genvar gi;
    generate
        for (gi = 0; gi < 3; gi++) begin : gen_mode
            vid_mode vid_mode (
                .clk(clk_sys),
                .rst_n(rst_n),
                .v(vid_v),
                .h(vid_h),
                .px_last(vid_px_last),
                .line_start(vid_line_start),
                .console(vid_console),
                .x_shift(vid_x_shift),
                .y_shift(vid_y_shift),
                .y_offset(vid_y_offset),
                .vid_mode_p_line(pm_line[gi]),
                .p_gnt(p_rotor == 2'(gi)),
                .p_entry(pm_entry),
                .p_config(pm_config),
                .vid_mode_a_req(ma_req[gi]),
                .vid_mode_a_addr(ma_addr[gi]),
                .vid_mode_f_req(mf_req[gi]),
                .vid_mode_f_addr(mf_addr[gi]),
                .f_gnt(f_any && f_sel == 2'(gi)),
                .f_data(font_bits),
                .a_gnt(a_any && a_sel == 3'(gi)),
                .a_rdata(xram_a_rdata),
                .vid_mode_pix(m_pix[gi]),
                .vid_mode_filled(m_filled[gi]),
                .vid_mode_busy(m_busy[gi]),
                .vid_mode_rnew(m_rnew[gi]),
                .vid_mode_rfilled(m_rfilled[gi]),
                .vid_mode_underrun(m_underrun[gi]),
                .sp_we(sp_we && sp_plane == 2'(gi)),
                .sp_addr(sp_addr),
                .sp_data(sp_data),
                .sp_force(sp_force && sp_plane == 2'(gi))
            );
        end
    endgenerate

    vid_sprite vid_sprite (
        .clk(clk_sys),
        .rst_n(rst_n),
        .v(vid_v),
        .h(vid_h),
        .line_start(vid_line_start),
        .console(vid_console),
        .x_shift(vid_x_shift),
        .y_shift(vid_y_shift),
        .y_offset(vid_y_offset),
        .vid_sprite_s_idx(sp_s_idx),
        .s_data(sp_s_data),
        .busy(m_busy),
        .rnew(m_rnew),
        .rfilled(m_rfilled),
        .vid_sprite_plane(sp_plane),
        .vid_sprite_we(sp_we),
        .vid_sprite_addr(sp_addr),
        .vid_sprite_data(sp_data),
        .vid_sprite_force(sp_force),
        .vid_sprite_overrun(sp_overrun),
        .vid_sprite_a_req(ma_req[3]),
        .vid_sprite_a_addr(ma_addr[3]),
        .a_gnt(a_any && a_sel == 3'd3),
        .a_rdata(xram_a_rdata),
        .ov_clear(sp_ov_clear)
    );

    /* Three device registers at the audio window: the PSG's pointer, the
     * console bell, and the OPL2's pointer. */
    logic aud_we;
    always_comb aud_we = bus_stb && bus_we && bus_sel_aud;

    logic signed [15:0] psg_l, psg_r;
    logic psg_valid;
    aud_psg aud_psg (
        .clk(clk_sys),
        .rst_n(rst_n),
        .xaddr_we(aud_we && bus_addr[3:2] == 2'b00),
        .xaddr_wdata(bus_wdata[15:0]),
        .aud_psg_a_req(ma_req[4]),
        .aud_psg_a_addr(ma_addr[4]),
        .a_gnt(a_any && a_sel == 3'd4),
        .a_rdata(xram_a_rdata),
        .q_we(xr_busy && xr_we),
        .q_addr(xr_addr),
        .q_val(xr_wdata),
        .aud_psg_l(psg_l),
        .aud_psg_r(psg_r),
        .aud_psg_valid(psg_valid)
    );

    /* One channel: the right output carries the same sample and nothing
     * downstream reads it any more. */
    logic signed [15:0] opl_l;
    logic opl_valid, opl_enabled;
    aud_opl aud_opl (
        .clk(clk_sys),
        .rst_n(rst_n),
        .xaddr_we(aud_we && bus_addr[3:2] == 2'b10),
        .xaddr_wdata(bus_wdata[15:0]),
        .q_we(xr_busy && xr_we),
        .q_addr(xr_addr),
        .q_val(xr_wdata),
        .aud_opl_out(opl_l),
        .aud_opl_valid(opl_valid),
        .aud_opl_enabled(opl_enabled)
    );

    /* The OPL2 into the codec's rate. A YM3812 samples every 1014 clk_sys
     * and the codec every 1050, so this is the one voice on the machine
     * whose rate is not ours to choose — everything else is generated at
     * 48 kHz to begin with. Before this the surplus was simply dropped:
     * pocket_i2s reloaded from whichever sample happened to be latest, so
     * about 1,700 OPL samples a second were overwritten and never
     * transmitted, unfiltered.
     *
     * One instance, not two. A YM3812 is mono and aud_opl emits the same
     * sample on both channels, so resampling it twice would buy nothing
     * and cost four more M10K. */
    logic signed [15:0] opl_rs;
    logic opl_rs_valid;
    aud_rsmp aud_rsmp (
        .clk(clk_sys),
        .rst_n(rst_n),
        .in_sample(opl_l),
        .in_valid(opl_valid),
        .aud_rsmp_out(opl_rs),
        .aud_rsmp_valid(opl_rs_valid)
    );

    /* The engines sum rather than select. The xreg validation lets only
     * one hold a pointer at a time and the other answers zero — the PSG
     * emits silence with its pointer parked, and aud_opl gates on its
     * own enable — so the sum is the selection, without the mux.
     *
     * The tick still follows the enabled engine, but no longer because
     * they run at different rates: both are 48 kHz now. The resampler
     * emits on whichever OPL sample completed an output interval, so its
     * pulses are evenly paced only on average, and the PSG's come off its
     * own divider. Same rate, different phase, so one of them has to be
     * the heartbeat rather than both. */
    logic signed [16:0] eng_l, eng_r;
    logic eng_valid;
    always_comb begin
        eng_l = 17'(opl_rs) + 17'(psg_l);
        eng_r = 17'(opl_rs) + 17'(psg_r);
        eng_valid = opl_enabled ? opl_rs_valid : psg_valid;
    end

    /* One bell for the machine, past the mux. Both engines used to carry
     * their own and only the selected one could ever be heard, so the
     * second cost 207 ALMs and a DSP to be inaudible.
     *
     * It keeps its own 48 kHz tick rather than riding whichever engine
     * won, because the two run at 24,000 and 49,704 and a bell stepped
     * by the winner would change pitch with the engine — which is the
     * bug the RATE parameter was just added to fix. 50.4 MHz over 1050
     * is exactly 48,000. The engine's tick still decides when the sum
     * reaches the codec; at 587 Hz the bell is far below what even the
     * slower engine can carry. */
    localparam int BEL_TICKS = 1050;
    logic [10:0] bel_div;
    logic bel_step;
    always_ff @(posedge clk_sys or negedge rst_n) begin
        if (!rst_n) begin
            bel_div <= '0;
            bel_step <= 1'b0;
        end else begin
            bel_step <= bel_div == 11'(BEL_TICKS - 1);
            bel_div <= bel_div == 11'(BEL_TICKS - 1) ? '0 : bel_div + 11'd1;
        end
    end

    logic signed [15:0] bel_out;
    aud_bel #(.RATE(48000)) bel (
        .clk(clk_sys),
        .rst_n(rst_n),
        .strike(aud_we && bus_addr[3:2] == 2'b01),
        .step(bel_step),
        .aud_bel_out(bel_out)
    );

    logic signed [17:0] out_l, out_r;
    always_comb begin
        out_l = 18'(eng_l) + 18'(bel_out);
        out_r = 18'(eng_r) + 18'(bel_out);
        rp6502_aud_l = out_l < -18'sd32768 ? -16'sd32768
            : out_l > 18'sd32767 ? 16'sd32767 : 16'(out_l);
        rp6502_aud_r = out_r < -18'sd32768 ? -16'sd32768
            : out_r > 18'sd32767 ? 16'sd32767 : 16'(out_r);
        rp6502_aud_valid = eng_valid;
    end

    /* The 180- and 360-line canvases sit under a 60-line letterbox. */
    logic vid_letterbox;
    always_comb vid_letterbox = vid_y_offset != 10'd0
        && (vid_v < 10'd60 || vid_v >= 10'd420);

    vid_compose vid_compose (
        .clk(clk_sys),
        .rst_n(rst_n),
        .de(vid_de),
        .console(vid_console),
        .letterbox(vid_letterbox),
        .term_pix(term_pix),
        .p0_pix(m_pix[0]),
        .p0_filled(m_filled[0]),
        .p1_pix(m_pix[1]),
        .p1_filled(m_filled[1]),
        .p2_pix(m_pix[2]),
        .p2_filled(m_filled[2]),
        .vid_compose_pix(rp6502_vid_pixel),
        .vid_compose_de(rp6502_vid_de)
    );

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_vid;
    always_comb unused_vid = ^{vid_hsync, vid_vsync, vid_vsync_pulse};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
