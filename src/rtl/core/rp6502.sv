/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The machine, independent of the FPGA platform hosting it.
 *
 * Two quirks the map inherits and nothing else would predict: every write
 * also lands in the SRAM shadow, and $FF00-$FFCF reads float at the last
 * value the bus carried.
 */

module rp6502
    import rp6502_pkg::*;
#(
    /* Empty in simulation, which loads the arrays through the bench. */
    parameter TCM_INIT_FILE = "",
    /* clk_sys in kHz, which the PHI2 accumulator counts against. */
    parameter int SYS_KHZ = 50400,
    /* Where the 6502's 64 KB lives. Zero builds it here out of block
     * memory, which is what every bench wants and what a platform with
     * blocks to spare should do. One exports the two ports and lets the
     * platform find the storage — on the Pocket that is a real SRAM
     * chip, and the 64 blocks it gives back stay with the fabric. */
    parameter bit EXT_RAM = 0
) (
    input logic clk_sys,
    /* Half clk_sys, rising with it. Made outside: a divider made here
     * would rise after this module's own registers settle on the same
     * edge, and a master clocked that late reads a ready the machine has
     * not published. */
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

    input logic slot_set,
    input logic [31:0] slot_len,
    /* How many times the host has announced a slot. Zero forever on a
     * platform that never announces one. */
    input logic [7:0] upd_n,
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

    /* The canvas names the scaler's mode; de already is the canvas. */
    output logic [15:0] rp6502_vid_pixel,
    output logic rp6502_vid_de,
    output logic [2:0] rp6502_vid_canvas,

    output logic signed [15:0] rp6502_aud_l,
    output logic signed [15:0] rp6502_aud_r,
    output logic rp6502_aud_valid,

    output logic [RP6502_SCANLINE_W-1:0] rp6502_scanline,
    output logic rp6502_vid_frame,

    /* The 6502's 64 KB when EXT_RAM says the platform holds it. Port A
     * is the machine's own and answers within the PHI2 period; port B is
     * the soft CPU's window and can be told to wait. Unconnected and
     * ignored when EXT_RAM is zero. */
    output logic [15:0] rp6502_ram_a_addr,
    output logic [7:0] rp6502_ram_a_wdata,
    output logic rp6502_ram_a_we,
    input logic [7:0] ram_a_rdata,
    output logic [15:0] rp6502_ram_b_addr,
    output logic [7:0] rp6502_ram_b_wdata,
    output logic rp6502_ram_b_we,
    output logic rp6502_ram_b_stb,
    input logic [7:0] ram_b_rdata,
    input logic ram_b_stall,
    /* The platform's request to stand the machine still for a cycle. */
    input logic ram_hold,
    /* What the platform's memory needs to know about the machine: the
     * cycle it is actually taking, and whether it is running at all.
     * This is the gated pulse — a cycle the hold suppressed is a cycle
     * the 6502 did not take, and must not be fetched for. */
    output logic rp6502_phi2_en,
    output logic rp6502_cpu_run
);

    /* The soft CPU measures time against its own clock, not this one. */
    localparam int RV_KHZ = SYS_KHZ / 2;

    logic [15:0] phi2_khz;
    logic phi2_raw_en, phi2_en;
    /* The platform can stand the machine still for a cycle — the soft
     * CPU's window onto the 6502's RAM shares one port with the 6502
     * when the RAM is off-chip, and something has to give way. Nothing
     * asks for this today: both users of that window run with the 6502
     * halted. It exists so that a firmware which did ask would be slow
     * rather than deadlocked. */
    always_comb phi2_en = phi2_raw_en && !ram_hold;
    always_comb rp6502_phi2_en = phi2_en;
    phi2_div #(.SYS_KHZ(SYS_KHZ)) phi2_div (
        .clk(clk_sys),
        .phi2_khz(phi2_khz),
        .phi2_div_en(phi2_raw_en)
    );

    logic [15:0] cpu_addr, cpu_next_addr;
    logic [7:0] cpu_dout, cpu_din, cpu_next_data;
    logic cpu_we, cpu_next_we;
    logic via_irq;
    // Opcode-fetch marker; the debug tap will want it, nothing does yet.
    logic cpu_sync;
    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_sync;
    /* verilator lint_on UNUSEDSIGNAL */
    always_comb unused_sync = cpu_sync;

    /* RESB inverted, reaching the 6502 and the 6522 and nothing else. A
     * register, not a gate of one with the platform's reset: the platform
     * already clears this flop asynchronously, so the gate would say
     * nothing the flop does not while putting a combinational term on a
     * reset network. */
    logic resb /*verilator public_flat_rw*/;
    always_comb rp6502_cpu_run = resb;
    logic cpu_stp;

    cpu65 cpu (
        .clk(clk_sys),
        .rst_n(resb),
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
        .cpu65_stp(cpu_stp),
        .cpu65_next_addr(cpu_next_addr),
        .cpu65_next_data(cpu_next_data),
        .cpu65_next_we(cpu_next_we)
    );

    logic sel_via, sel_ria, sel_open;
    always_comb begin
        sel_via = cpu_addr[15:4] == 12'hFFD;
        sel_ria = cpu_addr[15:5] == 11'b1111_1111_111;
        sel_open = cpu_addr[15:8] == 8'hFF && !sel_via && !sel_ria;
    end

    /* Every write lands in the shadow, whatever else it hits. */
    logic [7:0] sram_rdata;
    logic [7:0] sram_b_rdata;
    generate
        if (EXT_RAM) begin : g_ram_ext
            always_comb begin
                rp6502_ram_a_addr = cpu_next_addr;
                rp6502_ram_a_wdata = cpu_next_data;
                rp6502_ram_a_we = cpu_next_we;
                rp6502_ram_b_addr = bus_addr[15:0];
                rp6502_ram_b_wdata = bus_wbyte;
                rp6502_ram_b_we = bus_we;
                rp6502_ram_b_stb = bus_pend && bus_sel_sram;
                sram_rdata = ram_a_rdata;
                sram_b_rdata = ram_b_rdata;
            end
        end else begin : g_ram_bram
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
            always_comb begin
                rp6502_ram_a_addr = '0;
                rp6502_ram_a_wdata = '0;
                rp6502_ram_a_we = 1'b0;
                rp6502_ram_b_addr = '0;
                rp6502_ram_b_wdata = '0;
                rp6502_ram_b_we = 1'b0;
                rp6502_ram_b_stb = 1'b0;
            end
            /* verilator lint_off UNUSEDSIGNAL */
            logic unused_ram;
            always_comb unused_ram = ^{ram_a_rdata, ram_b_rdata,
                                       cpu_next_addr, cpu_next_data,
                                       cpu_next_we};
            /* verilator lint_on UNUSEDSIGNAL */
        end
    endgenerate

    logic ria_irq;
    logic [7:0] via_data;
    via via (
        .clk(clk_sys),
        .rst_n(resb),
        .en(phi2_en),
        .cs(sel_via),
        .we(cpu_we),
        .rs(cpu_addr[3:0]),
        .data_i(cpu_dout),
        .via_data(via_data),
        .via_irq(via_irq)
    );

    /* The soft CPU's clock is half this one, so every level it drives
     * stands for two machine clocks and every pulse the machine sends can
     * fall between two of its edges. Fixed here rather than in rv_soc,
     * which does not know it is clocked slowly: the strobe and the
     * console valid are narrowed to one machine clock, while slot_set and
     * key_set are held for two so an edge always sees them. */
    logic bus_stb_raw, bus_stb_n, bus_stb_q;
    logic rv_tx_valid_raw, rv_tx_valid_q;
    logic slot_set_q, key_set_q;

    /* The two clocks rise together, so a question asked on the rising
     * edge is asked while the answer is changing. bus_stb_raw is worse
     * than a crossing: rv_soc builds it out of its own bus_pend and the
     * machine's bus_rdy, a term from both clocks. Compared live against a
     * copy that caught the new value one edge early, it reads 1 && !1 —
     * the pulse never fires, and the whole access disappears silently.
     * Which accesses go depends on skew between two global networks, so
     * it is a different set every fit and none of it in simulation.
     *
     * On the falling edge the value has been still for half a period and
     * the comparison is between two registers on this clock. */
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
     * a later sample buys nothing and only narrows the pulse to a half
     * period, which rising-edge consumers then catch at its expiry. */
    always_comb rp6502_rv_tx_valid = rv_tx_valid_raw && !rv_tx_valid_q;

    logic bus_stb, bus_we, bus_pend;
    always_comb bus_stb = bus_stb_n && !bus_stb_q;

    /* Held until the request it answers goes away, because the other
     * clock looks only every second one. A register the analyzer can see,
     * where the term it replaces was a glitch it cannot. */
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

    /* RV_KHZ, not SYS_KHZ: mtime_acc is clocked by clk_rv, and a
     * microsecond is 25.2 of those — not whole. Ten per clock wrapping at
     * a hundredth of the rate keeps the fraction exact. Left at the
     * module's 1/1 the clock runs 25.2x fast, which nothing in simulation
     * waits on a real second to catch. */
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
        .upd_n(upd_n),
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
                 && !(bus_sel_stage && stage_stall)
                 && !(bus_sel_sram && ram_b_stall)),
        .bus_taken(bus_taken),
        .rv_soc_bus_pend(bus_pend),
        .rv_soc_bus_stb(bus_stb_raw),
        .rv_soc_bus_we(bus_we),
        .rv_soc_bus_addr(bus_addr),
        .rv_soc_bus_wdata(bus_wdata),
        .rv_soc_bus_wstrb(bus_wstrb),
        .bus_rdata(bus_rdata)
    );

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

    /* Shown early for a slow platform; the strobe-captured register
     * holds it through the answer cycle. */
    logic [27:0] stage_addr_q;
    always_comb begin
        rp6502_stage_pend = bus_pend && bus_sel_stage;
        rp6502_stage_addr = rp6502_stage_pend ? bus_addr[27:0]
                                              : stage_addr_q;
    end

    logic api_pending;
    logic bus_ctl_api, bus_vid_prog;
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

    /* Held at power-on and the firmware's from then on. The platform's
     * reset does not reach it: RESB is the OS's line, the way it is on
     * the RP2350, and cpu_init drives it low before anything else runs. */
    initial resb = 1'b0;
    always_ff @(posedge clk_sys)
        if (bus_stb && bus_we && bus_sel_ctl && !bus_addr[2])
            resb <= bus_wbyte[0];

    /* The byte-wide windows put their byte on every lane, so the master's
     * own extract picks the addressed one. */
    logic [7:0] bus_rbyte;
    always_comb begin
        case (bus_rsel)
            3'd2: bus_rbyte = bus_ctl_api ? {7'b0, api_pending}
                : {6'b0, cpu_stp, resb};
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

    initial bus_hold = 8'h00;
    always_ff @(posedge clk_sys)
        if (phi2_en && !cpu_we)
            bus_hold <= cpu_din;

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

    /* Port B is the RW engine's while busy, so the soft CPU's strobe
     * waits; the engine's background refresh yields to it in turn. */
    logic [7:0] xram_b_rdata;
    logic xr_busy, xr_we;
    logic [15:0] xr_addr;
    logic [7:0] xr_wdata;
    logic [31:0] xram_a_rdata;
    /* The terminal and a mode-1 fill share the font store, so the
     * rotor is real arbitration. Mod two, so the wrap is free. */
    logic [1:0] mf_req;
    logic [13:0] mf_addr[2];
    logic f_rotor, f_sel;
    logic f_any;
    always_comb begin
        f_sel = f_rotor;
        f_any = 1'b0;
        for (int i = 0; i < 2; i++) begin
            logic cand;
            cand = f_rotor ^ 1'(i);
            if (!f_any && mf_req[cand]) begin
                f_sel = cand;
                f_any = 1'b1;
            end
        end
    end
    initial f_rotor = 1'd0;
    always_ff @(posedge clk_sys)
        if (f_any)
            f_rotor <= f_sel + 1'd1;

    logic [7:0] font_bits;
    vid_font vid_font (
        .clk(clk_sys),
        .addr(mf_addr[f_sel]),
        .vid_font_bits(font_bits),
        .w_stb(bus_stb && bus_we && bus_sel_vid && bus_addr[18]),
        .w_addr(bus_addr[13:0]),
        .w_data(bus_wdata)
    );

    logic [2:0] ma_req;
    logic [13:0] ma_addr[3];
    logic [1:0] a_rotor, a_sel;
    logic a_any;
    /* Every rotor position solved at once, so the rotor only chooses
     * between answers that settled while it arrived from a register.
     * Scanning from the live rotor instead put a priority scan in
     * front of the address mux. The lowest offset with a request wins:
     * the loop counts down and the last assignment stands. */
    logic [1:0] sel_at[3];
    logic any_at[3];
    always_comb begin
        for (int r = 0; r < 3; r++) begin
            sel_at[r] = 2'(r);
            any_at[r] = 1'b0;
            for (int i = 2; i >= 0; i--) begin
                if (ma_req[(r + i) % 3]) begin
                    sel_at[r] = 2'((r + i) % 3);
                    any_at[r] = 1'b1;
                end
            end
        end
        a_sel = sel_at[a_rotor];
        a_any = any_at[a_rotor];
    end
    initial a_rotor = 2'd0;
    always_ff @(posedge clk_sys)
        if (a_any)
            a_rotor <= a_sel == 2'd2 ? 2'd0 : a_sel + 2'd1;
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
    logic [9:0] vid_cw, vid_ch;

    logic [8:0] sched_p_line;
    logic [1:0] sched_p_plane;
    logic [31:0] pm_entry;
    logic [15:0] pm_config;

    vid_prog vid_prog (
        .clk(clk_sys),
        .frame_start(vid_frame_start),
        .v(vid_v),
        .px_first(vid_px_first),
        .vid_prog_vsync_pulse(prog_vsync_pulse),
        .h(vid_h),
        .vid_prog_canvas(vid_canvas),
        .vid_prog_cw(vid_cw),
        .vid_prog_ch(vid_ch),
        .p_line(sched_p_line),
        .p_plane(sched_p_plane),
        .vid_prog_p_entry(pm_entry),
        .vid_prog_p_config(pm_config),
        .s_idx(sp_s_idx),
        .vid_prog_s_data(sp_s_data),
        .b_stb(bus_stb && bus_sel_vid && !bus_addr[18]
               && bus_addr[17]),
        .b_we(bus_we),
        .b_addr(bus_addr[15:0]),
        .b_wdata(bus_wdata),
        .vid_prog_b_rdata(vid_prog_b_rdata)
    );

    logic [15:0] mode0_pix;
    vid_mode0 vid_mode0 (
        .clk(clk_sys),
        .frame_start(vid_frame_start),
        .h(vid_h),
        .v(vid_v),
        .px_last(vid_px_last),
        .line_start(vid_line_start),
        .vid_mode0_pix(mode0_pix),
        .vid_mode0_f_req(mf_req[1]),
        .vid_mode0_f_addr(mf_addr[1]),
        .f_gnt(f_any && f_sel == 1'd1),
        .f_data(font_bits),
        .b_stb(bus_stb && bus_sel_vid && !bus_addr[18]
               && !bus_addr[17]),
        .b_we(bus_we),
        .b_addr(bus_addr[16:0]),
        .b_wstrb(bus_wstrb),
        .b_wdata(bus_wdata),
        .vid_mode0_b_rdata(vid_b_rdata)
    );

    logic [15:0] m_pix[3];
    logic [12:0] sp_s_idx;
    logic [31:0] sp_s_data;
    logic [16:0] sp_pix[3];

    logic fl_start;
    logic [2:0] fl_mode;
    logic [15:0] fl_attr, fl_config;
    logic fl_done;
    logic fl_px_we;
    logic [9:0] fl_px_addr;
    logic [15:0] fl_px_data;
    logic [2:0] m_px_we;
    logic [2:0] m_done;
    logic [2:0] sched_term;
    vid_sched vid_sched (
        .clk(clk_sys),
        .rst_n(rst_n),
        .v(vid_v),
        .h(vid_h),
        .line_start(vid_line_start),
        .cw(vid_cw),
        .ch(vid_ch),
        .vid_sched_p_line(sched_p_line),
        .vid_sched_p_plane(sched_p_plane),
        .p_entry(pm_entry),
        .p_config(pm_config),
        .vid_sched_e_start(fl_start),
        .vid_sched_e_mode(fl_mode),
        .vid_sched_e_attr(fl_attr),
        .vid_sched_e_config(fl_config),
        .e_done(fl_done),
        .e_px_we(fl_px_we),
        .vid_sched_px_we(m_px_we),
        .vid_sched_done(m_done),
        .vid_sched_term(sched_term)
    );
    vid_fill vid_fill (
        .clk(clk_sys),
        .line_start(vid_line_start),
        .start(fl_start),
        .mode(fl_mode),
        .attr_i(fl_attr),
        .config_ptr_i(fl_config),
        .t_row(sched_p_line),
        .cw(vid_cw),
        .vid_fill_a_req(ma_req[0]),
        .vid_fill_a_addr(ma_addr[0]),
        .a_gnt(a_any && a_sel == 2'd0),
        .a_rdata(xram_a_rdata),
        .vid_fill_f_req(mf_req[0]),
        .vid_fill_f_addr(mf_addr[0]),
        .f_gnt(f_any && f_sel == 1'd0),
        .f_data(font_bits),
        .vid_fill_px_we(fl_px_we),
        .vid_fill_px_addr(fl_px_addr),
        .vid_fill_px_data(fl_px_data),
        .vid_fill_done(fl_done)
    );
    genvar gi;
    generate
        for (gi = 0; gi < 3; gi++) begin : gen_mode
            vid_mode vid_mode (
                .clk(clk_sys),
                .h(vid_h),
                .px_last(vid_px_last),
                .line_start(vid_line_start),
                .px_we(m_px_we[gi]),
                .px_addr(fl_px_addr),
                .px_data(fl_px_data),
                .done_i(m_done[gi]),
                .vid_mode_pix(m_pix[gi])
            );
        end
    endgenerate

    /* verilator lint_off PINCONNECTEMPTY */
    vid_sprite vid_sprite (
        .clk(clk_sys),
        .v(vid_v),
        .h(vid_h),
        .px_last(vid_px_last),
        .line_start(vid_line_start),
        .cw(vid_cw),
        .ch(vid_ch),
        .vid_sprite_s_idx(sp_s_idx),
        .s_data(sp_s_data),
        .vid_sprite_pix(sp_pix),
        .vid_sprite_overrun(),
        .vid_sprite_a_req(ma_req[1]),
        .vid_sprite_a_addr(ma_addr[1]),
        .a_gnt(a_any && a_sel == 2'd1),
        .a_rdata(xram_a_rdata)
    );
    /* verilator lint_on PINCONNECTEMPTY */

    /* Four bits of offset rather than two, so the bell's descriptor has
     * somewhere to live beside the two pointers. */
    logic aud_we;
    always_comb aud_we = bus_stb && bus_we && bus_sel_aud;

    logic psg_tick;

    logic signed [15:0] psg_l, psg_r;
    /* verilator lint_off PINCONNECTEMPTY */
    aud_psg aud_psg (
        .clk(clk_sys),
        .xaddr_we(aud_we && bus_addr[5:2] == 4'h0),
        .xaddr_wdata(bus_wdata[15:0]),
        .aud_psg_a_req(ma_req[2]),
        .aud_psg_a_addr(ma_addr[2]),
        .a_gnt(a_any && a_sel == 2'd2),
        .a_rdata(xram_a_rdata),
        .q_we(xr_busy && xr_we),
        .q_addr(xr_addr),
        .q_val(xr_wdata),
        .bel_lo_we(aud_we && bus_addr[5:2] == 4'h4),
        .bel_hi_we(aud_we && bus_addr[5:2] == 4'h5),
        .bel_wdata(bus_wdata),
        .aud_psg_l(psg_l),
        .aud_psg_r(psg_r),
        /* The machine runs off the tick below, which is the divider and
         * not the walk. */
        .aud_psg_valid(),
        .aud_psg_tick(psg_tick)
    );
    /* verilator lint_on PINCONNECTEMPTY */

    logic signed [15:0] opl_l;
    logic opl_valid;
    /* verilator lint_off PINCONNECTEMPTY */
    aud_opl aud_opl (
        .clk(clk_sys),
        .xaddr_we(aud_we && bus_addr[5:2] == 4'h2),
        .xaddr_wdata(bus_wdata[15:0]),
        .q_we(xr_busy && xr_we),
        .q_addr(xr_addr),
        .q_val(xr_wdata),
        .aud_opl_out(opl_l),
        .aud_opl_valid(opl_valid),
        .aud_opl_enabled()
    );
    /* verilator lint_on PINCONNECTEMPTY */

    /* A YM3812 samples every 1014 clk_sys and the codec every 1050, so
     * this is the one voice whose rate is not ours to choose. Dropping
     * the surplus instead lost about 1,700 samples a second, unfiltered.
     *
     * One instance: a YM3812 is mono, so resampling it twice would buy
     * nothing and pay a second filter's coefficient store for it. */
    logic signed [15:0] opl_rs;
    /* verilator lint_off PINCONNECTEMPTY */
    aud_rsmp aud_rsmp (
        .clk(clk_sys),
        .in_sample(opl_l),
        .in_valid(opl_valid),
        .step(psg_tick),
        .aud_rsmp_out(opl_rs),
        /* Pulled, so its answer is ready when the tick comes round. */
        .aud_rsmp_valid()
    );
    /* verilator lint_on PINCONNECTEMPTY */

    /* Nothing selects and nothing gates: an engine making no sound
     * contributes zero. One heartbeat, the PSG's divider — the tick
     * itself, not the end of a walk, whose length moves with the XRAM
     * rotor. The OPL's resampler is pulled by the same tick, so every
     * voice reaching the codec was taken on the same clock. */
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

    /* de IS the canvas: its rows on the first canvas_h lines, its own
     * width, and the scaler does the rest. */
    always_comb vid_de = vid_de_full && vid_h < vid_cw && vid_v < vid_ch;

    /* A mode-0 slot swaps the plane's fill stream for the terminal
     * engine's; sprites still merge on top. */
    logic [15:0] c_pix[3];
    always_comb
        for (int i = 0; i < 3; i++)
            c_pix[i] = sched_term[i] ? mode0_pix : m_pix[i];
    vid_compose vid_compose (
        .clk(clk_sys),
        .de(vid_de),
        .p0_pix(c_pix[0]),
        .s0_pix(sp_pix[0]),
        .p1_pix(c_pix[1]),
        .s1_pix(sp_pix[1]),
        .p2_pix(c_pix[2]),
        .s2_pix(sp_pix[2]),
        .vid_compose_pix(rp6502_vid_pixel),
        .vid_compose_de(rp6502_vid_de)
    );

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_vid;
    always_comb unused_vid = ^{vid_hsync, vid_vsync, vid_vsync_pulse};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
