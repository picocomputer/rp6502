/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

module wiring
    import timing_pkg::*, mem_pkg::*;
#(
    parameter int SYS_KHZ = 50400
) (
    input logic clk_sys,
    /* The host is writing a new ROM: the 6502 goes into reset and stays
     * there while this is high. */
    input logic rom_stop,
    /* XRAM's render port runs at twice clk_sys; xram.sv says how, and
     * why clk_ph rides with it. */
    input logic clk_a2,
    input logic clk_ph,
    input logic rst_n,

    output logic [7:0] wiring_tx_data,
    output logic wiring_tx_valid,
    input logic rx_valid,
    input logic [7:0] rx_data,
    output logic wiring_rx_taken,

    output logic [7:0] wiring_rv_tx_data,
    output logic wiring_rv_tx_valid,
    output logic wiring_rv_halted,
    output logic [31:0] wiring_rv_exit_code,

    /* The staging store, the memory the cache does not hold. An access
     * is taken on the first clock with wiring_stage_pend high and
     * stage_stall low. For a read, stage_word has to hold the four bytes
     * at wiring_stage_addr with its low two bits dropped, the lowest
     * address in bits 7 to 0, on that clock, so a slow platform holds
     * stage_stall high until the word is there. A write puts the bytes
     * of wiring_stage_wdata that wiring_stage_wstrb names at the same
     * word, laid out the same way, and a read after it is given them. */
    output logic [27:0] wiring_stage_addr,
    output logic wiring_stage_pend,
    output logic wiring_stage_we,
    output logic [31:0] wiring_stage_wdata,
    output logic [3:0] wiring_stage_wstrb,
    input logic stage_stall,
    input logic [31:0] stage_word,

    /* The soft CPU's memory, a line of four words at a time. A line is
     * 128 bits with word 0 in bits 31 to 0, and wiring_line_addr counts
     * lines from the soft CPU's address 0. wiring_line_pend is high while a
     * read or a write is wanted and not yet done, and the other three
     * hold with it. line_done rises when it is done, with line_rdata
     * valid for a read, and falls once wiring_line_pend has fallen;
     * line_rdata holds until the next request is taken. */
    output logic wiring_line_pend,
    output logic wiring_line_we,
    output logic [MEM_AW-3:0] wiring_line_addr,
    output logic [127:0] wiring_line_wdata,
    input logic line_done,
    input logic [127:0] line_rdata,

    /* wiring_host_stb is a one-clock strobe, and host_rdata must be
     * valid from the clock after it until the soft CPU reads it, two
     * clocks after the strobe. */
    output logic [27:0] wiring_host_addr,
    output logic wiring_host_stb,
    output logic wiring_host_we,
    output logic [31:0] wiring_host_wdata,
    input logic [31:0] host_rdata,

    input logic slot_set,
    input logic [31:0] slot_len,
    input logic [7:0] upd_n,
    input logic key_set,
    input logic [8:0] key_code,
    input logic [3:0][31:0] cont_key,
    input logic [3:0][31:0] cont_joy,
    input logic [3:0][15:0] cont_trig,
    output logic wiring_key_pending,

    output logic [15:0] wiring_vid_pixel,
    output logic wiring_vid_de,
    output logic [2:0] wiring_vid_canvas,

    output logic signed [15:0] wiring_aud_l,
    output logic signed [15:0] wiring_aud_r,
    output logic wiring_aud_valid,

    output logic [SCANLINE_W-1:0] wiring_scanline,
    output logic wiring_vid_frame
);

    logic [15:0] phi2_khz, phi2_acc;
    logic phi2_en, phi2_pulse;

    phi2 #(.SYS_KHZ(SYS_KHZ)) phi2 (
        .clk(clk_sys),
        .phi2_khz(phi2_khz),
        .hold(held),
        .st_jam(st_jam),
        .st_jam_acc(jam_acc),
        .st_jam_pulse(jam_pulse),
        .phi2_en(phi2_en),
        .phi2_acc(phi2_acc),
        .phi2_pulse(phi2_pulse)
    );

    logic [15:0] cpu_addr;
    logic [7:0] cpu_dout, cpu_din;
    logic cpu_we;
    logic via_irq;

    logic resb /*verilator public_flat_rw*/;
    logic cpu_stp;

    /* The machine's savestate registers, the soft CPU's window at
     * 0x90000000, by soc_addr[13] and then soc_addr[6:2]:
     *
     *   0x00     MACH_HOLD. Bit 0 written set asks for the hold, and the
     *            machine stops on the first clock its audio engines are
     *            quiet: the 6502's enable, the audio engines' sample
     *            counters, the RIA's vsync and mtime stand still
     *            together, and the engines stay quiet. Bit 0 reads what
     *            was asked, and bit 1 is set while the hold is in force.
     *            Bit 0 written clear lets them all go on the same clock.
     *   0x04     ST_SEL. The group in bits 4:3, 0 MACH, 1 the 6502,
     *            2 the VIA, 3 the bus, and the word in bits 2:0.
     *   0x08     ST_DATA. The word ST_SEL picks. MACH word 0 is the
     *            pending PHI2 enable in bit 1 and RESB in bit 0, and
     *            word 1 the PHI2 accumulator in bits 31:16 and the rate
     *            in bits 15:0. The bus's word 0 is the open-bus byte.
     *   0x0C     JAM. A write, while the hold is in force, loads the
     *            JAM_DATA words into the 6502, the VIA, the bus and the
     *            PHI2 count together. The 6502 and the VIA take theirs
     *            only out of reset, so RESB is written first.
     *   0x10     REGS_RESTORE. Bit 0 set has the RIA window's writes put
     *            back what its reads gave; regs.sv says which words
     *            differ.
     *   0x14     AUD_OWN. Bit 0 set gives the audio engines' memories to
     *            AUD_SST. It is taken only while the hold is in force,
     *            and the hold's end drops it.
     *   0x40     JAM_DATA, fifteen words, laid out as ST_DATA reads them:
     *            0-4 the 6502's words, 5-11 the VIA's, 12-13 MACH's, 14
     *            the bus's.
     *   0x2000   AUD_SST, 2048 words, the audio engines' state at
     *            psg.sv's, opl.sv's and pcm.sv's savestate addresses,
     *            read and written while AUD_OWN is set.
     *
     * ST_DATA and AUD_SST read the machine as it is, so a savestate reads
     * them with the hold in force. */
    localparam int JAM_VIA = 5, JAM_MACH = 12, JAM_BUS = 14;
    /* The jam is held this many clocks, the multicycle the timing
     * constraints give paths into the 6502 and the VIA. It loads the
     * same words on each, with the PHI2 enable held low. */
    localparam int JAM_CLOCKS = 4;
    logic hold_req, regs_restore, aud_own, st_jam;
    logic held /*verilator public_flat_rd*/;
    logic [4:0] st_sel;
    logic [2:0] jam_left;
    logic [31:0] jam_cpu[5], jam_via[7];
    logic [15:0] jam_acc;
    logic [7:0] jam_bus;
    logic jam_pulse;
    logic aud_quiet;
    always_comb st_jam = jam_left != 3'd0;

    logic mach_we, aud_sst_we;
    always_comb begin
        mach_we = soc_stb && soc_we && soc_sel_mach && !soc_addr[13];
        aud_sst_we = soc_stb && soc_we && soc_sel_mach && soc_addr[13]
            && aud_own;
    end
    initial begin
        hold_req = 1'b0;
        held = 1'b0;
        regs_restore = 1'b0;
        aud_own = 1'b0;
        st_sel = '0;
        jam_left = '0;
    end
    always_ff @(posedge clk_sys) begin
        held <= hold_req && (held || aud_quiet);
        if (st_jam)
            jam_left <= jam_left - 3'd1;
        if (mach_we && soc_addr[6]) begin
            if (soc_addr[5:2] < 4'(JAM_VIA))
                jam_cpu[3'(soc_addr[5:2])] <= soc_wdata;
            else if (soc_addr[5:2] < 4'(JAM_MACH))
                jam_via[3'(soc_addr[5:2] - 4'(JAM_VIA))] <= soc_wdata;
            else if (soc_addr[5:2] == 4'(JAM_MACH))
                jam_pulse <= soc_wdata[1];
            else if (soc_addr[5:2] == 4'(JAM_MACH + 1))
                jam_acc <= soc_wdata[31:16];
            else if (soc_addr[5:2] == 4'(JAM_BUS))
                jam_bus <= soc_wdata[7:0];
        end else if (mach_we)
            case (soc_addr[4:2])
                3'd0: hold_req <= soc_wdata[0];
                3'd1: st_sel <= soc_wdata[4:0];
                3'd3: if (held) jam_left <= 3'(JAM_CLOCKS);
                3'd4: regs_restore <= soc_wdata[0];
                3'd5: aud_own <= soc_wdata[0] && held;
                default: ;
            endcase
        if (!held)
            aud_own <= 1'b0;
    end

    logic [31:0] cpu_st_rdata, via_st_rdata, mach_st_rdata, st_rdata;
    logic [7:0] bus_hold;
    always_comb begin
        case (st_sel[2:0])
            3'd0: mach_st_rdata = {30'd0, phi2_pulse, resb};
            3'd1: mach_st_rdata = {phi2_acc, phi2_khz};
            default: mach_st_rdata = '0;
        endcase
        case (st_sel[4:3])
            2'd0: st_rdata = mach_st_rdata;
            2'd1: st_rdata = cpu_st_rdata;
            2'd2: st_rdata = via_st_rdata;
            default: st_rdata = {24'd0, bus_hold};
        endcase
    end

    logic [31:0] mach_rdata;
    always_comb
        case (soc_addr[4:2])
            3'd0: mach_rdata = {30'd0, held, hold_req};
            3'd1: mach_rdata = {27'd0, st_sel};
            3'd2: mach_rdata = st_rdata;
            3'd4: mach_rdata = {31'd0, regs_restore};
            3'd5: mach_rdata = {31'd0, aud_own};
            default: mach_rdata = '0;
        endcase

    logic ria_hold;
    cpu cpu (
        .clk(clk_sys),
        .rst_n(resb),
        .en(phi2_en),
        .data_i(cpu_din),
        .irq_i(via_irq || ria_irq),
        .nmi_i(1'b0),
        .rdy_i(ria_hold), /* an EXIT waits here for the reset */
        .res_i(1'b0),
        .cpu_addr(cpu_addr),
        .cpu_data(cpu_dout),
        .cpu_we(cpu_we),
        .cpu_stp(cpu_stp),
        .st_idx(st_sel[2:0]),
        .cpu_st_rdata(cpu_st_rdata),
        .st_jam(st_jam),
        .st_jam_data(jam_cpu)
    );

    logic sel_via, sel_ria;

    /* Every 6502 write also lands in RAM, including a write to
     * $FF00-$FFFF, where 6502 reads come from the VIA, the RIA or open
     * bus instead. */
    logic [7:0] sram_rdata;
    logic [7:0] sram_b_rdata;
    sram sram (
        .clk(clk_sys),
        .a_addr(cpu_addr),
        .a_wdata(cpu_dout),
        .a_we(cpu_we && phi2_en),
        .sram_a_rdata(sram_rdata),
        .b_addr(soc_addr[15:0]),
        .b_wdata(soc_wbyte),
        .b_we(soc_stb && soc_we && soc_sel_sram),
        .sram_b_rdata(sram_b_rdata)
    );

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
        .via_irq(via_irq),
        .st_idx(st_sel[2:0]),
        .via_st_rdata(via_st_rdata),
        .st_jam(st_jam),
        .st_jam_data(jam_via)
    );

    /* An access is taken on the first clock that finds it pending and
     * its target able to finish it: XRAM once its port is free, the
     * staging store once it stops stalling, everything else at once.
     * soc_stb is the request while it waits. soc_taken is the take a
     * clock late, so the soft CPU ends the access a clock after that and
     * XRAM's byte, two clocks behind the take, is in xram_b_hold. */
    logic soc_stb, soc_pend, soc_we;
    logic soc_taken, soc_take;
    initial soc_taken = 1'b0;
    always_comb soc_stb = soc_pend && !soc_taken;
    always_comb soc_take = soc_stb
        && (soc_sel_xram ? xram_go
            : soc_sel_stage ? !stage_stall : 1'b1);
    always_ff @(posedge clk_sys)
        soc_taken <= soc_take;
    logic [31:0] soc_addr, soc_wdata;
    logic [3:0] soc_wstrb;
    logic [31:0] soc_rdata;

    /* mtime stops and goes on with the 6502, on the same clock. */
    soc #(.CLK_KHZ(SYS_KHZ)) soc (
        .clk(clk_sys),
        .rst_n(rst_n),
        .soc_phi2_khz(phi2_khz),
        .mtime_hold(held),
        .soc_line_pend(wiring_line_pend),
        .soc_line_we(wiring_line_we),
        .soc_line_addr(wiring_line_addr),
        .soc_line_wdata(wiring_line_wdata),
        .line_done(line_done),
        .line_rdata(line_rdata),
        .soc_tx_data(wiring_rv_tx_data),
        .soc_tx_valid(wiring_rv_tx_valid),
        .soc_halted(wiring_rv_halted),
        .soc_exit_code(wiring_rv_exit_code),
        .slot_set(slot_set),
        .slot_len(slot_len),
        .upd_n(upd_n),
        .key_set(key_set),
        .cont_key(cont_key),
        .cont_joy(cont_joy),
        .cont_trig(cont_trig),
        .key_code(key_code),
        .soc_key_pending(wiring_key_pending),
        .bus_taken(soc_taken),
        .soc_bus_pend(soc_pend),
        .soc_bus_we(soc_we),
        .soc_bus_addr(soc_addr),
        .soc_bus_wdata(soc_wdata),
        .soc_bus_wstrb(soc_wstrb),
        .bus_rdata(soc_rdata)
    );

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_bus;
    always_comb unused_bus = ^{soc_addr[27:21]};
    /* verilator lint_on UNUSEDSIGNAL */
    logic [7:0] soc_wbyte;
    always_comb begin
        soc_wbyte = soc_wdata[7:0];
        if (soc_wstrb[1])
            soc_wbyte = soc_wdata[15:8];
        if (soc_wstrb[2])
            soc_wbyte = soc_wdata[23:16];
        if (soc_wstrb[3])
            soc_wbyte = soc_wdata[31:24];
    end

    logic soc_sel_sram, soc_sel_regs, soc_sel_ctl, soc_sel_stage,
        soc_sel_vid, soc_sel_xram, soc_sel_aud, soc_sel_host, soc_sel_mach;
    always_comb begin
        soc_sel_sram = soc_addr[31:28] == 4'h1;
        soc_sel_xram = soc_addr[31:28] == 4'h3;
        soc_sel_regs = soc_addr[31:28] == 4'h2;
        soc_sel_ctl = soc_addr[31:28] == 4'h4;
        soc_sel_stage = soc_addr[31:28] == 4'h6;
        soc_sel_vid = soc_addr[31:28] == 4'h5;
        soc_sel_aud = soc_addr[31:28] == 4'h7;
        soc_sel_host = soc_addr[31:28] == 4'h8;
        soc_sel_mach = soc_addr[31:28] == 4'h9;
    end

    always_comb begin
        wiring_host_addr = soc_addr[27:0];
        wiring_host_stb = soc_stb && soc_sel_host;
        wiring_host_we = soc_we;
        wiring_host_wdata = soc_wdata;
    end

    /* wiring_stage_pend is high from the soft CPU's request until the
     * clock that takes it, so a slow platform can fetch the word before
     * then, and a write is taken once. That clock captures the word in
     * stage_q, so the platform owes nothing after it. */
    logic [31:0] stage_q;
    always_comb begin
        wiring_stage_pend = soc_stb && soc_sel_stage;
        wiring_stage_addr = soc_addr[27:0];
        wiring_stage_we = soc_we;
        wiring_stage_wdata = soc_wdata;
        wiring_stage_wstrb = soc_wstrb;
    end

    logic api_pending;
    logic soc_ctl_api, vid_prog_q;
    logic [31:0] regs_b_rdata, regs_b_q;
    logic [31:0] mode0_b_rdata, prog_b_rdata;
    logic [31:0] aud_rdata, aud_q, mach_q, aud_sst_rdata, aud_sst_q;
    logic [3:0] soc_rsel;
    initial begin
        soc_rsel = 4'd0;
        soc_ctl_api = 1'b0;
        vid_prog_q = 1'b0;
        stage_q = '0;
        aud_q = '0;
        mach_q = '0;
    end
    always_ff @(posedge clk_sys) begin
        if (soc_stb) begin
            soc_rsel <= soc_sel_regs ? 4'd1
                : soc_sel_ctl ? 4'd2
                : soc_sel_stage ? 4'd3
                : soc_sel_vid ? 4'd4
                : soc_sel_xram ? 4'd5
                : soc_sel_host ? 4'd6
                : soc_sel_aud ? 4'd7
                : soc_sel_mach ? (soc_addr[13] ? 4'd9 : 4'd8)
                : 4'd0;
            soc_ctl_api <= soc_addr[2];
            vid_prog_q <= soc_addr[17];
            stage_q <= stage_word;
            /* regs_b_q is captured at the strobe, because a read of word
             * 16 of the regs window pops the console queue on this edge,
             * a read of word 26 takes back the 6502's unread byte, and a
             * read of word 27 clears SIGINT, after which regs_b_rdata
             * shows what is left. */
            regs_b_q <= regs_b_rdata;
            /* The same for the OPL's drop count and PCM's underruns, which
             * their reads clear. */
            aud_q <= aud_rdata;
            mach_q <= mach_rdata;
        end
    end
    /* The audio engines' memories read a clock after their address, so
     * AUD_SST is taken on every clock and the soft CPU reads it a clock
     * or more after its strobe. */
    always_ff @(posedge clk_sys)
        aud_sst_q <= aud_sst_rdata;

    /* The 6502 is in reset while the core is, so no program runs on
     * without its firmware, and while the host writes a new ROM. */
    initial resb = 1'b0;
    always_ff @(posedge clk_sys)
        if (!rst_n || rom_stop) resb <= 1'b0;
        else if (soc_stb && soc_we && soc_sel_ctl && !soc_addr[2])
            resb <= soc_wbyte[0];

    /* The byte-wide windows put their byte on all four lanes, because a
     * Hazard3 byte load picks its lane from the low address bits. The
     * staging store returns its whole word, as memory does. */
    logic [7:0] soc_rbyte;
    always_comb begin
        case (soc_rsel)
            4'd2: soc_rbyte = soc_ctl_api ? {7'b0, api_pending}
                : {6'b0, cpu_stp, resb};
            4'd5: soc_rbyte = xram_b_hold;
            default: soc_rbyte = sram_b_rdata;
        endcase
        case (soc_rsel)
            4'd1: soc_rdata = regs_b_q;
            4'd3: soc_rdata = stage_q;
            4'd4: soc_rdata = vid_prog_q ? prog_b_rdata : mode0_b_rdata;
            4'd6: soc_rdata = host_rdata;
            4'd7: soc_rdata = aud_q;
            4'd8: soc_rdata = mach_q;
            4'd9: soc_rdata = aud_sst_q;
            default: soc_rdata = {4{soc_rbyte}};
        endcase
    end

    logic [7:0] ria_data;
    regs ria (
        .clk(clk_sys),
        .en(phi2_en),
        .cs(sel_ria),
        .we(cpu_we),
        .rs(cpu_addr[4:0]),
        .data_i(cpu_dout),
        .regs_data(ria_data),
        .regs_tx_data(wiring_tx_data),
        .regs_tx_valid(wiring_tx_valid),
        .rx_valid(rx_valid),
        .rx_data(rx_data),
        .regs_rx_taken(wiring_rx_taken),
        /* Held with the 6502, so it resumes at the frame count and the
         * interrupt it stopped at. */
        .vsync_pulse(prog_vsync_pulse && !held),
        .regs_irq(ria_irq),
        .regs_xr_busy(xr_busy),
        .regs_xr_we(xr_we),
        .regs_xr_addr(xr_addr),
        .regs_xr_wdata(xr_wdata),
        .xr_rdata(xram_b_rdata),
        .xr_cpu_want(soc_pend && soc_sel_xram),
        .sst_restore(regs_restore),
        .b_we(soc_stb && soc_we && soc_sel_regs),
        .b_re(soc_stb && !soc_we && soc_sel_regs),
        .b_word(soc_addr[9:2]),
        .b_wstrb(soc_wstrb),
        .b_wdata(soc_wdata),
        .regs_b_rdata(regs_b_rdata),
        .regs_api_pending(api_pending),
        .api_ack(soc_stb && soc_we && soc_sel_ctl && soc_addr[2]),
        .regs_hold(ria_hold)
    );

    bus bus (
        .clk(clk_sys),
        .en(phi2_en),
        .addr(cpu_addr),
        .we(cpu_we),
        .dout(cpu_dout),
        .ria_data(ria_data),
        .via_data(via_data),
        .sram_rdata(sram_rdata),
        .bus_din(cpu_din),
        .bus_sel_via(sel_via),
        .bus_sel_ria(sel_ria),
        .bus_hold(bus_hold),
        .st_jam(st_jam),
        .st_jam_hold(jam_bus)
    );

    logic [9:0] vid_h /*verilator public_flat_rd*/;
    logic [9:0] vid_v /*verilator public_flat_rd*/;
    logic vid_de_full;
    logic vid_de /*verilator public_flat_rd*/;
    logic vid_hsync /*verilator public_flat_rd*/;
    logic vid_vsync /*verilator public_flat_rd*/;
    logic vid_line_start, vid_frame_start;
    always_comb wiring_vid_frame = vid_frame_start;
    logic vid_vsync_pulse;
    logic prog_vsync_pulse;
    logic vid_px_first, vid_px_last;
    timing timing (
        .clk(clk_sys),
        .timing_h(vid_h),
        .timing_v(vid_v),
        .timing_px_first(vid_px_first),
        .timing_px_last(vid_px_last),
        .timing_de(vid_de_full),
        .timing_hsync(vid_hsync),
        .timing_vsync(vid_vsync),
        .timing_line_start(vid_line_start),
        .timing_frame_start(vid_frame_start),
        .timing_vsync_pulse(vid_vsync_pulse)
    );
    always_comb wiring_scanline = vid_v;

    /* The RW engine in regs.sv, which serves the 6502's RW0 and RW1
     * registers, holds XRAM port B while xr_busy is high. Its background
     * refresh yields to a pending soft CPU access, but an access the 6502
     * started does not, so a soft CPU access to XRAM is not taken until
     * xr_busy drops. */
    logic [7:0] xram_b_rdata;
    logic xr_busy, xr_we;
    logic [15:0] xr_addr;
    logic [7:0] xr_wdata;
    logic [31:0] xram_f_rdata, xram_s_rdata;
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
    font font (
        .clk(clk_sys),
        .addr(mf_addr[f_sel]),
        .font_bits(font_bits),
        .w_stb(soc_stb && soc_we && soc_sel_vid && soc_addr[18]),
        .w_addr(soc_addr[13:0]),
        .w_data(soc_wdata)
    );

    /* The fill and the sprite stage each own a slot on XRAM's render
     * port, a word every clock, so a request is always taken: the port
     * reads whatever address each presents, and the word is on that
     * reader's data port two clocks after. */
    logic [1:0] ma_req /*verilator public_flat_rd*/;
    logic [13:0] ma_addr[2];

    logic xram_owed;
    initial xram_owed = 1'b0;
    always_ff @(posedge clk_sys) begin
        if (!soc_pend)
            xram_owed <= 1'b0;
        else if (soc_stb && soc_sel_xram && xr_busy)
            xram_owed <= 1'b1;
        else if (xram_owed && !xr_busy)
            xram_owed <= 1'b0;
    end
    logic xram_go;
    always_comb xram_go = !xr_busy
        && ((soc_stb && soc_sel_xram) || xram_owed);

    /* The XRAM writes on port B, the RW engine's and the soft CPU's. The
     * PSG and the OPL snoop the RW engine's alone, the 6502's writes
     * through RW0 and RW1, as on every machine; a write the OS makes, such
     * as read_xram's, reaches XRAM only. */
    logic xw_we;
    logic [15:0] xw_addr;
    logic [7:0] xw_wdata;
    always_comb begin
        xw_we = xr_busy ? xr_we : (xram_go && soc_we);
        xw_addr = xr_busy ? xr_addr : soc_addr[15:0];
        xw_wdata = xr_busy ? xr_wdata : soc_wbyte;
    end

    /* XRAM port B registers its read, so its byte arrives one clock
     * after the address, and the RW engine can take port B on that
     * clock. The byte for a soft CPU read is therefore captured one
     * clock after xram_go. */
    logic xram_cap;
    logic [7:0] xram_b_hold;
    initial begin
        xram_cap = 1'b0;
        xram_b_hold = '0;
    end
    always_ff @(posedge clk_sys) begin
        xram_cap <= xram_go && !soc_we;
        if (xram_cap)
            xram_b_hold <= xram_b_rdata;
    end
    /* The snooped write is registered because the soft CPU's side of the
     * xw mux is combinational from its bus and the PSG forwards a
     * snooped byte combinationally into its voice arithmetic, so without
     * the register one path would run from the soft CPU's bus through
     * that arithmetic in a single clk_sys period. Neither the PSG nor
     * the OPL reads XRAM, so a one-clock delay on the snooped write
     * cannot put either out of step with the array. A savestate takes
     * the machine with this write long since landed. */
    logic qs_we;
    logic [15:0] qs_addr;
    logic [7:0] qs_val;
    initial begin
        qs_we = 1'b0;
        qs_addr = '0;
        qs_val = '0;
    end
    always_ff @(posedge clk_sys)
        {qs_we, qs_addr, qs_val} <= {xw_we && xr_busy, xw_addr, xw_wdata};
    xram xram (
        .clk(clk_sys),
        .clk_a2(clk_a2),
        .clk_ph(clk_ph),
        .f_addr(ma_addr[0]),
        .xram_f_rdata(xram_f_rdata),
        .s_addr(ma_addr[1]),
        .xram_s_rdata(xram_s_rdata),
        .b_addr(xw_addr),
        .b_wdata(xw_wdata),
        .b_we(xw_we),
        .xram_b_rdata(xram_b_rdata)
    );

    logic [2:0] vid_canvas;
    always_comb wiring_vid_canvas = vid_canvas;
    logic [9:0] vid_cw, vid_ch;

    logic [8:0] sched_p_line;
    logic [1:0] sched_p_plane;
    logic [31:0] pm_entry;
    logic [15:0] pm_config;

    prog prog (
        .clk(clk_sys),
        .frame_start(vid_frame_start),
        .v(vid_v),
        .px_first(vid_px_first),
        .prog_vsync_pulse(prog_vsync_pulse),
        .h(vid_h),
        .prog_canvas(vid_canvas),
        .prog_cw(vid_cw),
        .prog_ch(vid_ch),
        .p_line(sched_p_line),
        .p_plane(sched_p_plane),
        .prog_p_entry(pm_entry),
        .prog_p_config(pm_config),
        .s_idx(sp_s_idx),
        .prog_s_data(sp_s_data),
        .b_stb(soc_stb && soc_sel_vid && !soc_addr[18]
               && soc_addr[17]),
        .b_we(soc_we),
        .b_addr(soc_addr[15:0]),
        .b_wdata(soc_wdata),
        .prog_b_rdata(prog_b_rdata)
    );

    /* The beam's derived columns, computed once for the line buffers:
     * the sprite buffers' erase a pixel behind the beam, and the read for
     * the next pixel, which wraps at the line's end. */
    logic vid_h_last;
    logic [9:0] vid_sc_addr, vid_rd_addr;
    always_comb begin
        vid_h_last = vid_h == 10'd799;
        vid_sc_addr = vid_h - 10'd1;
        vid_rd_addr = vid_h_last ? 10'd0 : vid_h + 10'd1;
    end

    /* The row map, derived once in sched.sv and read by every stage. */
    logic [9:0] sched_t;
    logic sched_dbl, sched_pair_start, sched_pair_end, sched_render_now;
    logic sched_term_go;
    logic [15:0] sched_term_cfg;

    logic [15:0] mode0_pix;
    mode0 mode0 (
        .clk(clk_sys),
        .frame_start(vid_frame_start),
        .h(vid_h),
        .v(vid_v),
        .px_last(vid_px_last),
        .line_start(vid_line_start),
        .cw(vid_cw),
        .t(sched_t),
        .pair_start(sched_pair_start),
        .pair_end(sched_pair_end),
        .term_go(sched_term_go),
        .term_cfg(sched_term_cfg),
        .mode0_pix(mode0_pix),
        .mode0_f_req(mf_req[1]),
        .mode0_f_addr(mf_addr[1]),
        .f_gnt(f_any && f_sel == 1'd1),
        .f_data(font_bits),
        .b_stb(soc_stb && soc_sel_vid && !soc_addr[18]
               && !soc_addr[17]),
        .b_we(soc_we),
        .b_sel(soc_addr[20] ? 2'd3 : soc_addr[19] ? 2'd2
               : soc_addr[16] ? 2'd1 : 2'd0),
        .b_addr(soc_addr[15:0]),
        .b_wstrb(soc_wstrb),
        .b_wdata(soc_wdata),
        .mode0_b_rdata(mode0_b_rdata)
    );

    logic [15:0] m_pix[3];
    logic [12:0] sp_s_idx;
    logic [31:0] sp_s_data;
    logic [16:0] sp_pix[3];

    logic fl_start;
    logic [2:0] fl_mode;
    logic [15:0] fl_attr, fl_config;
    logic fl_done;
    logic [1:0] fl_px_we;
    logic [9:0] fl_px_addr;
    logic [31:0] fl_px_data;
    logic [1:0] m_px_we[3];
    logic [2:0] m_done;
    logic [2:0] sched_term;
    sched sched (
        .clk(clk_sys),
        .rst_n(rst_n),
        .v(vid_v),
        .h(vid_h),
        .line_start(vid_line_start),
        .cw(vid_cw),
        .ch(vid_ch),
        .sched_t(sched_t),
        .sched_dbl(sched_dbl),
        .sched_pair_start(sched_pair_start),
        .sched_pair_end(sched_pair_end),
        .sched_render_now(sched_render_now),
        .sched_p_line(sched_p_line),
        .sched_p_plane(sched_p_plane),
        .p_entry(pm_entry),
        .p_config(pm_config),
        .sched_e_start(fl_start),
        .sched_e_mode(fl_mode),
        .sched_e_attr(fl_attr),
        .sched_e_config(fl_config),
        .e_done(fl_done),
        .e_px_we(fl_px_we),
        .sched_px_we(m_px_we),
        .sched_done(m_done),
        .sched_term(sched_term),
        .sched_term_go(sched_term_go),
        .sched_term_cfg(sched_term_cfg)
    );
    fill fill (
        .clk(clk_sys),
        .row_start(vid_row_start),
        .start(fl_start),
        .mode(fl_mode),
        .attr_i(fl_attr),
        .config_ptr_i(fl_config),
        .t_row(sched_p_line),
        .cw(vid_cw),
        .fill_a_req(ma_req[0]),
        .fill_a_addr(ma_addr[0]),
        .a_gnt(ma_req[0]),
        .a_rdata(xram_f_rdata),
        .fill_f_req(mf_req[0]),
        .fill_f_addr(mf_addr[0]),
        .f_gnt(f_any && f_sel == 1'd0),
        .f_data(font_bits),
        .fill_px_we(fl_px_we),
        .fill_px_addr(fl_px_addr),
        .fill_px_data(fl_px_data),
        .fill_done(fl_done)
    );
    genvar gi;
    generate
        for (gi = 0; gi < 3; gi++) begin : gen_mode
            linebuf linebuf (
                .clk(clk_sys),
                .rd_addr(vid_rd_addr),
                .h_last(vid_h_last),
                .px_last(vid_px_last),
                .line_start(vid_line_start),
                .flip_ok(vid_flip_ok),
                .next_ok(vid_next_ok),
                .px_we(m_px_we[gi]),
                .px_addr(fl_px_addr),
                .px_data(fl_px_data),
                .done_i(m_done[gi]),
                .linebuf_pix(m_pix[gi])
            );
        end
    endgenerate

    sprite sprite (
        .clk(clk_sys),
        .sc_addr(vid_sc_addr),
        .rd_addr(vid_rd_addr),
        .h_last(vid_h_last),
        .px_last(vid_px_last),
        .line_start(vid_line_start),
        .cw(vid_cw),
        .t_row(sched_t[8:0]),
        .render_now(sched_render_now),
        .pair_start(sched_pair_start),
        .pair_end(sched_pair_end),
        .sprite_s_idx(sp_s_idx),
        .s_data(sp_s_data),
        .sprite_pix(sp_pix),
        .sprite_a_req(ma_req[1]),
        .sprite_a_addr(ma_addr[1]),
        .a_rdata(xram_s_rdata)
    );

    logic aud_we, aud_rd;
    always_comb begin
        aud_we = soc_stb && soc_we && soc_sel_aud;
        aud_rd = soc_stb && !soc_we && soc_sel_aud;
    end

    /* AUD_SST's address and data are the soft CPU's. The engines' flop
     * words take a write on the same strobe as their memories, and each
     * engine takes only the words at its own addresses. */
    logic [10:0] aud_sst_addr;
    always_comb aud_sst_addr = soc_addr[12:2];
    logic [31:0] psg_sst_rdata, opl_sst_rdata, pcm_sst_rdata;
    logic psg_quiet, opl_quiet, pcm_quiet;
    always_comb begin
        aud_sst_rdata = psg_sst_rdata | opl_sst_rdata | pcm_sst_rdata;
        aud_quiet = psg_quiet && opl_quiet && pcm_quiet;
    end

    /* The audio window's readable words, by soc_addr[5:2]. */
    logic psg_busy;
    logic [30:0] opl_drops, pcm_under;
    logic [12:0] pcm_level;
    always_comb
        case (soc_addr[5:2])
            4'h1: aud_rdata = {31'd0, psg_busy};
            4'h3: aud_rdata = {1'b0, opl_drops};
            4'hB: aud_rdata = {19'd0, pcm_level};
            4'hC: aud_rdata = {1'b0, pcm_under};
            default: aud_rdata = '0;
        endcase

    logic psg_tick;

    logic signed [15:0] psg_l, psg_r;
    /* verilator lint_off PINCONNECTEMPTY */
    psg psg (
        .clk(clk_sys),
        .xaddr_we(aud_we && soc_addr[5:2] == 4'h0),
        .xaddr_wdata(soc_wdata[15:0]),
        .q_we(qs_we),
        .q_addr(qs_addr),
        .q_val(qs_val),
        .bel_we(aud_we && soc_addr[5:2] == 4'h4),
        .bel_idx(soc_wdata[10:8]),
        .bel_val(soc_wdata[7:0]),
        .psg_l(psg_l),
        .psg_r(psg_r),
        .psg_valid(),
        .psg_tick(psg_tick),
        .psg_busy(psg_busy),
        .sst_own(aud_own),
        .sst_addr(aud_sst_addr),
        .sst_we(aud_sst_we),
        .sst_wdata(soc_wdata),
        .sst_park(held),
        .psg_sst_rdata(psg_sst_rdata),
        .psg_quiet(psg_quiet)
    );
    /* verilator lint_on PINCONNECTEMPTY */

    logic signed [15:0] opl_l;
    logic opl_valid;
    /* verilator lint_off PINCONNECTEMPTY */
    opl opl (
        .clk(clk_sys),
        .xaddr_we(aud_we && soc_addr[5:2] == 4'h2),
        .xaddr_wdata(soc_wdata[15:0]),
        .q_we(qs_we),
        .q_addr(qs_addr),
        .q_val(qs_val),
        .drops_take(aud_rd && soc_addr[5:2] == 4'h3),
        .opl_out(opl_l),
        .opl_valid(opl_valid),
        .opl_enabled(),
        .opl_drops(opl_drops),
        .sst_own(aud_own),
        .sst_addr(aud_sst_addr),
        .sst_we(aud_sst_we),
        .sst_wdata(soc_wdata),
        .sst_park(held),
        .opl_sst_rdata(opl_sst_rdata),
        .opl_quiet(opl_quiet)
    );
    /* verilator lint_on PINCONNECTEMPTY */

    /* The OPL produces a sample every 1014 clocks and the audio output
     * takes one on each psg_tick, every 1050 clocks, so the OPL output
     * is resampled onto the tick, on one filter with the PCM: device. One
     * stream serves both channels because a YM3812 is mono.
     *
     * The PCM: device, by soc_addr[5:2]: 6 control, 7 step, 8 four
     * bytes, 9 one byte, 10 clear, 13 commit; 11 reads the level and 12
     * the underruns, which the read clears. */
    localparam int PCM_W = rsmp_coef_pkg::RSMP_OUT_W;
    logic signed [15:0] opl_rs;
    logic signed [PCM_W-1:0] pcm_l, pcm_r;
    /* verilator lint_off PINCONNECTEMPTY */
    pcm pcm (
        .clk(clk_sys),
        .tick(psg_tick),
        .ctrl_we(aud_we && soc_addr[5:2] == 4'h6),
        .step_we(aud_we && soc_addr[5:2] == 4'h7),
        .data_we(aud_we && soc_addr[5:2] == 4'h8),
        .byte_we(aud_we && soc_addr[5:2] == 4'h9),
        .clear_we(aud_we && soc_addr[5:2] == 4'hA),
        .commit_we(aud_we && soc_addr[5:2] == 4'hD),
        .wdata(soc_wdata),
        .under_take(aud_rd && soc_addr[5:2] == 4'hC),
        .pcm_level(pcm_level),
        .pcm_under(pcm_under),
        .pcm_l(pcm_l),
        .pcm_r(pcm_r),
        .in_sample(opl_l),
        .in_valid(opl_valid),
        .step(psg_tick),
        .rsmp_out(opl_rs),
        /* rsmp_valid is unused because opl_rs holds its value until the
         * audio output takes it on the next tick. */
        .rsmp_valid(),
        .sst_own(aud_own),
        .sst_addr(aud_sst_addr),
        .sst_we(aud_sst_we),
        .pcm_sst_rdata(pcm_sst_rdata),
        .pcm_quiet(pcm_quiet)
    );
    /* verilator lint_on PINCONNECTEMPTY */

    /* Two 16-bit engines and pcm's PCM_W bits add in PCM_W + 1. */
    localparam int ENG_W = PCM_W + 1;
    localparam logic signed [ENG_W-1:0] ENG_MIN = ENG_W'(-32768),
        ENG_MAX = ENG_W'(32767);
    logic signed [ENG_W-1:0] eng_l, eng_r;
    always_comb begin
        eng_l = ENG_W'(opl_rs) + ENG_W'(psg_l) + ENG_W'(pcm_l);
        eng_r = ENG_W'(opl_rs) + ENG_W'(psg_r) + ENG_W'(pcm_r);
        wiring_aud_l = eng_l < ENG_MIN ? -16'sd32768
            : eng_l > ENG_MAX ? 16'sd32767 : 16'(eng_l);
        wiring_aud_r = eng_r < ENG_MIN ? -16'sd32768
            : eng_r > ENG_MAX ? 16'sd32767 : 16'(eng_r);
        wiring_aud_valid = psg_tick;
    end

    /* A 320 wide canvas spans two lines of timing per row of graphics, so
     * the row is handed to the scaler on the first line of the pair and the
     * second is blanked. The scaler still receives ch rows; what changed is
     * that the beam now takes the whole frame to cross the canvas. */
    /* A line buffer flips where a pair starts, and next_ok says the line
     * about to start is one; a row of graphics starts where a pair does. */
    logic vid_dbl, vid_flip_ok, vid_next_ok, vid_row_start;
    always_comb vid_dbl = sched_dbl;
    always_comb vid_flip_ok = sched_pair_start;
    always_comb vid_next_ok = sched_pair_end;
    always_comb vid_row_start = vid_line_start && sched_pair_start;
    always_comb vid_de = vid_de_full && vid_h < vid_cw
        && (vid_dbl ? !vid_v[0] && vid_v < {vid_ch[8:0], 1'b0}
                    : vid_v < vid_ch);

    logic [15:0] c_pix[3];
    always_comb
        for (int i = 0; i < 3; i++)
            c_pix[i] = sched_term[i] ? mode0_pix : m_pix[i];
    compose compose (
        .clk(clk_sys),
        .de(vid_de),
        .p0_pix(c_pix[0]),
        .s0_pix(sp_pix[0]),
        .p1_pix(c_pix[1]),
        .s1_pix(sp_pix[1]),
        .p2_pix(c_pix[2]),
        .s2_pix(sp_pix[2]),
        .compose_pix(wiring_vid_pixel),
        .compose_de(wiring_vid_de)
    );

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_vid;
    always_comb unused_vid = ^{vid_hsync, vid_vsync, vid_vsync_pulse};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
