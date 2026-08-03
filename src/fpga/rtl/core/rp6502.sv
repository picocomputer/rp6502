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

    /* One stereo sample per PSG tick, signed at full scale. */
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

    /* RESB, inverted: the one reset the machine has, held by the OS and
     * reaching the 6502 and the 6522 and nothing else. It is a register,
     * not a gate of one with the platform's reset — that would put a
     * combinational term on a reset network, and the platform already
     * reaches this flop asynchronously, so a gate would say nothing the
     * flop does not.
     *
     * A ROM load therefore hands the new program a VIA with its timers,
     * interrupt enables and port directions cleared, not the last
     * program's. */
    logic cpu_run /*verilator public_flat_rw*/;
    logic cpu_stp;

    cpu65 cpu (
        .clk(clk_sys),
        .rst_n(cpu_run),
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
        .rst_n(cpu_run),
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
    logic bus_stb_raw, bus_stb_n, bus_stb_q;
    logic rv_tx_valid_raw, rv_tx_valid_q;
    logic slot_set_q, key_set_q;

    /* These two narrowings ask a question about a signal from the other
     * clock, and the two clocks rise together, so asking it on the
     * rising edge asks it at the moment the answer is changing.
     *
     * bus_stb_raw is worse than a clock crossing: rv_soc builds it
     * combinationally out of its own bus_pend and the machine's
     * bus_rdy, so it is a term from both clocks with a glitch of its
     * own. It was compared live against its clk_sys copy, and the
     * failure that arrangement has is silent. Let the copy catch the
     * new value one edge early and the comparison reads 1 && !1: the
     * pulse never fires, and by the next edge both terms are high so it
     * never fires again. The whole bus access disappears with nothing
     * to show for it — a dropped byte at $FFE1, an API call at $FFF0
     * that never lands, an xreg write the video never sees. Which
     * accesses go depends on the skew between two global networks, so
     * it is a different set every fit and none of it in simulation.
     *
     * Take them on the falling edge instead. The launch is at the
     * rising edge the two clocks share, so half a period later the
     * value has been still for 9.9 ns rather than for nothing, and the
     * comparison is between two registers on this clock. The access is
     * still captured on the same rising edge it always was: the pulse
     * spans the half period from here to there. */
    /* The two halves are one mechanism and must stay one. Asynchronous
     * clear is a control signal a LAB shares, so a half that needs it
     * and a half that does not cannot be packed together: give them
     * different control and the fitter is free to separate them, which
     * is the separation this whole arrangement exists to deny. They
     * carry no state worth keeping, so neither takes the reset. */
    initial bus_stb_n = 1'b0;
    always_ff @(negedge clk_sys) begin
        bus_stb_n <= bus_stb_raw;
    end
    initial begin
        bus_stb_q = 1'b0;
        rv_tx_valid_q = 1'b0;
        slot_set_q = 1'b0;
        key_set_q = 1'b0;
    end
    always_ff @(posedge clk_sys) begin
        bus_stb_q <= bus_stb_n;
        rv_tx_valid_q <= rv_tx_valid_raw;
        slot_set_q <= slot_set;
        key_set_q <= key_set;
    end
    /* This one stays on the rising edge. rv_tx_valid_raw is a clean flop
     * from the soft CPU's clock — no bus_rdy, no term from this clock, so
     * nothing settles late and a later sample buys nothing. Taking it on
     * the falling edge only narrowed the pulse to the half period between
     * the two edges, which every rising-edge consumer then catches at its
     * expiry: pocket_dbg and pocket_dbglog both sample there, and the
     * bench stopped seeing the soft CPU's console at all. */
    always_comb rp6502_rv_tx_valid = rv_tx_valid_raw && !rv_tx_valid_q;

    logic bus_stb, bus_we, bus_pend;
    always_comb bus_stb = bus_stb_n && !bus_stb_q;

    /* What the soft CPU is told, rather than what it would work out for
     * itself. The strobe is one clock of this clock, and the other clock
     * looks only every second one, so it is held until the request it
     * answers goes away — a level that has been still for a whole period
     * by the time the far side reads it, and a register the analyzer can
     * see, where the term it replaces was a glitch that it cannot. */
    logic bus_taken;
    initial bus_taken = 1'b0;
    always_ff @(posedge clk_sys) begin
        if (!bus_pend)
            bus_taken <= 1'b0;
        else if (bus_stb)
            bus_taken <= 1'b1;
    end
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
        .bus_taken(bus_taken),
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
    initial begin
        bus_rsel = 3'd0;
        bus_ctl_api = 1'b0;
        bus_vid_prog = 1'b0;
        stage_addr_q = '0;
    end
    always_ff @(posedge clk_sys) begin
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
    end

    /* The 6502's RESB keeps the reset the rest of the machine gave up.
     * The platform asserts this whenever the host asks for it, not only
     * at power-on, and a 6502 that comes out of it still running is one
     * executing the last program against cells the firmware is in the
     * middle of rebuilding. */
    always_ff @(posedge clk_sys or negedge rst_n) begin
        if (!rst_n)
            cpu_run <= 1'b0;
        else if (bus_stb && bus_we && bus_sel_ctl && !bus_addr[2])
            cpu_run <= bus_wbyte[0];
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
    logic vid_de_full;
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
        .vid_timing_de(vid_de_full),
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

    /* The audio window: the PSG's pointer, the OPL2's pointer, and the two
     * words of the ninth voice the soft CPU rings the bell with. Four bits
     * of offset rather than two, so the window is sixty-four bytes and the
     * bell's descriptor has somewhere to live. */
    logic aud_we;
    always_comb aud_we = bus_stb && bus_we && bus_sel_aud;

    logic psg_tick;

    logic signed [15:0] psg_l, psg_r;
    /* verilator lint_off PINCONNECTEMPTY */
    aud_psg aud_psg (
        .clk(clk_sys),
        .rst_n(rst_n),
        .xaddr_we(aud_we && bus_addr[5:2] == 4'h0),
        .xaddr_wdata(bus_wdata[15:0]),
        .aud_psg_a_req(ma_req[4]),
        .aud_psg_a_addr(ma_addr[4]),
        .a_gnt(a_any && a_sel == 3'd4),
        .a_rdata(xram_a_rdata),
        .q_we(xr_busy && xr_we),
        .q_addr(xr_addr),
        .q_val(xr_wdata),
        .bel_lo_we(aud_we && bus_addr[5:2] == 4'h4),
        .bel_hi_we(aud_we && bus_addr[5:2] == 4'h5),
        .bel_wdata(bus_wdata),
        .aud_psg_l(psg_l),
        .aud_psg_r(psg_r),
        /* The walk's own done strobe. The machine runs off the tick
         * below instead, which is the divider and not the walk. */
        .aud_psg_valid(),
        .aud_psg_tick(psg_tick)
    );
    /* verilator lint_on PINCONNECTEMPTY */

    /* One channel: the right output carries the same sample and nothing
     * downstream reads it any more. */
    logic signed [15:0] opl_l;
    logic opl_valid;
    /* verilator lint_off PINCONNECTEMPTY */
    aud_opl aud_opl (
        .clk(clk_sys),
        .rst_n(rst_n),
        .xaddr_we(aud_we && bus_addr[5:2] == 4'h2),
        .xaddr_wdata(bus_wdata[15:0]),
        .q_we(xr_busy && xr_we),
        .q_addr(xr_addr),
        .q_val(xr_wdata),
        .aud_opl_out(opl_l),
        .aud_opl_valid(opl_valid),
        /* Nothing gates on it: an engine with no program answers zero and
         * a voice that is silent makes no sound. */
        .aud_opl_enabled()
    );
    /* verilator lint_on PINCONNECTEMPTY */

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
    /* verilator lint_off PINCONNECTEMPTY */
    aud_rsmp aud_rsmp (
        .clk(clk_sys),
        .rst_n(rst_n),
        .in_sample(opl_l),
        .in_valid(opl_valid),
        .step(psg_tick),
        .aud_rsmp_out(opl_rs),
        /* Pulled, so its answer is ready when the tick that asked for it
         * comes round again; the strobe is the module's own business. */
        .aud_rsmp_valid()
    );
    /* verilator lint_on PINCONNECTEMPTY */

    /* The engines sum. Nothing selects, nothing gates: an engine that is
     * not making sound contributes zero, and the console bell is the PSG's
     * ninth voice so it sums with the rest.
     *
     * One heartbeat, the PSG's divider — the tick itself, not the end of a
     * walk, whose length moves with the XRAM rotor and jumps when a pointer
     * is programmed. The OPL's resampler is pulled by the same tick, so
     * every voice reaching the codec was taken on the same clock. */
    logic signed [16:0] eng_l, eng_r;
    always_comb begin
        eng_l = 17'(opl_rs) + 17'(psg_l);
        eng_r = 17'(opl_rs) + 17'(psg_r);
        rp6502_aud_l = eng_l < -17'sd32768 ? -16'sd32768
            : eng_l > 17'sd32767 ? 16'sd32767 : 16'(eng_l);
        rp6502_aud_r = eng_r < -17'sd32768 ? -16'sd32768
            : eng_r > 17'sd32767 ? 16'sd32767 : 16'(eng_r);
        rp6502_aud_valid = psg_tick;
    end

    /* vid_timing's de is the full 640x480 window; the machine's de is
     * the CANVAS — each of its columns once, each of its rows once, and
     * nothing where the 180- and 360-line canvases used to carry a
     * letterbox. Doubling and letterboxing were presentation, and
     * presentation belongs to the platform: a sink that wants a fixed
     * 640x480 raster repeats pixels and lines for free, while
     * un-repeating them costs a buffer. This machine emits every canvas
     * pixel exactly once. */
    always_comb begin
        vid_de = vid_de_full;
        if (vid_x_shift)
            vid_de = vid_de && vid_h < 10'd320;
        if (vid_y_shift)
            vid_de = vid_de && !vid_v[0];
        if (vid_y_offset != 10'd0)
            vid_de = vid_de && vid_v >= 10'd60 && vid_v < 10'd420;
    end

    vid_compose vid_compose (
        .clk(clk_sys),
        .rst_n(rst_n),
        .de(vid_de),
        .console(vid_console),
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
