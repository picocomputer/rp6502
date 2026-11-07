/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The OPL2, bit-exact at its output word with YM3812-LLE, nukeykt's model
 * of the die, as core/aud/opl.c is. Each sample runs the reference
 * algorithm of core/aud/opl.md section 10 one slot at a time, every field
 * read from the register file, so nothing is cached.
 *
 * Writes to the 256 bytes wait in a queue of 256 and apply in order, at most one
 * before a sample, at the chip's pace (opl.md 7.8). A pointer write empties
 * the queue at once, and the next pop resets the engine instead.
 *
 * A savestate reads and writes the whole chip through the sst port at
 * savestate offsets: the queue at 0x400, the register file at 0x500, the
 * slots at 0x680, the channels at 0x6C0 and the flops at 0x780. The
 * arrays' write ports run on clk_sys, and so do the slot and channel
 * reads, the flops on clk. Between frames
 * every other flop and every registered read is dead, so only that state
 * is kept.
 */

module opl
    import opl_rom_pkg::*;
#(
    /* 50.4 MHz / 1014 is 49,704 samples a second. */
    parameter int TICKS_PER_SAMPLE = 1014,
    /* Clear for a bench with no clk_sys: the arrays' ports then run on
     * clk. */
    parameter bit SST_CLK = 1'b1
) (
    input logic clk,
    input logic clk_sys,

    /* Where the 256 bytes start, or 0xFFFF. */
    input logic xaddr_we,
    input logic [15:0] xaddr_wdata,

    /* Every write the 6502 makes to XRAM through RW0 or RW1. */
    input logic q_we,
    input logic [15:0] q_addr,
    input logic [7:0] q_val,

    /* Clears opl_drops on this clock. */
    input logic drops_take,

    /* A YM3812 is mono. */
    output logic signed [15:0] opl_out,
    output logic opl_valid,
    output logic opl_enabled,
    /* Writes that found the queue full, saturating at 0x7FFFFFFF. */
    output logic [30:0] opl_drops,

    /* sst_own gives the arrays' ports to sst_addr, written on sst_we.
     * sst_st_we writes the flop word at sst_addr on clk. sst_park holds
     * the sample counter, so no frame starts. */
    input logic sst_own,
    input logic [10:0] sst_addr,
    input logic sst_we,
    input logic sst_st_we,
    input logic [31:0] sst_wdata,
    input logic sst_park,
    output logic [31:0] opl_sst_rdata,
    /* Idle, with no frame due for QUIET_LEAD clocks. */
    output logic opl_quiet
);

    logic clk_a;
    assign clk_a = SST_CLK ? clk_sys : clk;

    logic sel_q, sel_r, sel_s, sel_c, sel_st;
    logic [2:0] st_k;
    always_comb begin
        sel_q = sst_addr[10:8] == 3'h4;
        sel_r = sst_addr[10:8] == 3'h5;
        sel_s = sst_addr[10:5] == 6'h34;
        sel_c = sst_addr[10:4] == 7'h6C;
        sel_st = sst_addr[10:3] == 8'hF0;
        st_k = sst_addr[2:0];
    end

    /* A stop lands within a few clocks of opl_quiet, and no frame may
     * start before it does. */
    localparam int QUIET_LEAD = 24;

    localparam logic [1:0] EG_ATT = 2'd0;
    localparam logic [1:0] EG_DEC = 2'd1;
    localparam logic [1:0] EG_SUS = 2'd2;
    localparam logic [1:0] EG_REL = 2'd3;
    /* A slot word is {state, level, phase accumulator}. */
    localparam logic [29:0] SLOT_RESET = {EG_REL, 9'h1FF, 19'd0};

    localparam logic [6:0] KSL_ROM[16] = '{
        7'd0, 7'd32, 7'd40, 7'd45, 7'd48, 7'd51, 7'd53, 7'd55,
        7'd56, 7'd58, 7'd59, 7'd60, 7'd61, 7'd62, 7'd63, 7'd64
    };
    /* Twice MULT. */
    localparam logic [4:0] MT[16] = '{
        5'd1, 5'd2, 5'd4, 5'd6, 5'd8, 5'd10, 5'd12, 5'd14,
        5'd16, 5'd18, 5'd20, 5'd20, 5'd24, 5'd24, 5'd30, 5'd30
    };

    localparam int TICK_W = $clog2(TICKS_PER_SAMPLE);
    initial if (TICK_W > 10)
        $fatal(1, "opl: the sample counter outgrew its flop word");

    /* ---------------- pointer and queue ---------------- */

    /* Where the 256 bytes start; register n is byte n of them. */
    logic [15:0] xaddr /*verilator public_flat_rd*/;
    logic enabled /*verilator public_flat_rd*/;
    always_comb opl_enabled = enabled;

    /* opl.c's opl_regs: 01, 08, BD, the operator and channel registers
     * and the channel aliases. */
    function automatic logic is_reg(logic [7:0] a);
        is_reg = a == 8'h01 || a == 8'h08 || a == 8'hBD
            || ((a[7:4] == 4'hA || a[7:4] == 4'hB || a[7:4] == 4'hC)
                && (a[3:0] <= 4'd11 || a[3:0] >= 4'd14))
            || ((a[7:5] == 3'd1 || a[7:5] == 3'd2 || a[7:5] == 3'd3
                 || a[7:5] == 3'd4 || a[7:5] == 3'd7)
                && a[4:0] < 5'd22 && a[2:0] < 3'd6);
    endfunction

    /* Entries are reg | val << 8, the oldest at q_head. */
    (* ramstyle = "M10K, no_rw_check" *)
    logic [15:0] queue[256];
    logic [7:0] q_head, q_tail;
    logic [31:0] drops_inc;
    always_comb drops_inc = {1'b0, opl_drops} + 32'd1;
    logic [8:0] q_count /*verilator public_flat_rd*/;
    logic q_take /*verilator public_flat_rd*/;
    logic snoop;
    logic [15:0] q_off;
    logic q_push /*verilator public_flat_rd*/;
    logic q_drop /*verilator public_flat_rd*/;
    always_comb begin
        q_off = q_addr - xaddr;
        snoop = q_we && enabled && q_off[15:8] == 8'd0 && is_reg(q_off[7:0]);
        q_push = snoop && !q_count[8];
        q_drop = snoop && q_count[8];
    end
    logic [7:0] qa_a;
    logic [15:0] q_rd, qa_q;
    always_comb qa_a = sst_own ? sst_addr[7:0] : q_tail;
    always_ff @(posedge clk_a) begin
        if (sst_own ? sst_we && sel_q : q_push)
            queue[qa_a] <= sst_own ? sst_wdata[15:0] : {q_val, q_off[7:0]};
        qa_q <= queue[qa_a];
    end
    always_ff @(posedge clk)
        q_rd <= queue[q_head];

    initial begin
        xaddr = 16'd0;
        enabled = 1'b0;
        q_head = '0;
        q_tail = '0;
        q_count = '0;
        opl_drops = '0;
        for (int i = 0; i < 256; i++)
            queue[i] = '0;
    end
    always_ff @(posedge clk) begin
        if (xaddr_we) begin
            xaddr <= xaddr_wdata;
            enabled <= xaddr_wdata != 16'hFFFF;
            q_head <= '0;
            q_tail <= '0;
            q_count <= '0;
            opl_drops <= '0;
        end else begin
            if (q_push)
                q_tail <= q_tail + 8'd1;
            if (q_take)
                q_head <= q_head + 8'd1;
            q_count <= q_count + {8'd0, q_push} - {8'd0, q_take};
            if (drops_take)
                opl_drops <= {30'd0, q_drop};
            else if (q_drop && !drops_inc[31])
                opl_drops <= drops_inc[30:0];
        end
        if (sst_st_we && sel_st)
            case (st_k)
                3'd0: {enabled, q_tail, q_head, xaddr[15:8]} <= sst_wdata[24:0];
                3'd1: opl_drops <= sst_wdata[30:0];
                3'd2: q_count <= sst_wdata[8:0];
                3'd7: xaddr[7:0] <= sst_wdata[7:0];
                default: ;
            endcase
    end

    /* ---------------- memories ---------------- */

    /* The register file at the chip's addresses, but for the views of
     * opl.md 7.6: the modulators of channels 0-2 read A9-AB and B9-BB, and
     * channels 0-5 take CON and FB from C9-CE. Those are copies of A0-A2,
     * B0-B2 and C0-C5 that follow a sample late, but for the CON of C3-C5,
     * which is copied at once. */
    (* ramstyle = "M10K, no_rw_check" *)
    logic [7:0] regs[256] /*verilator public_flat_rd*/;
    logic [7:0] rr_a, rw_a, rw_d, regs_q, ra_a, ra_q;
    logic rw_we;
    always_comb ra_a = sst_own ? sst_addr[7:0] : rw_a;
    always_ff @(posedge clk_a) begin
        if (sst_own ? sst_we && sel_r : rw_we)
            regs[ra_a] <= sst_own ? sst_wdata[7:0] : rw_d;
        ra_q <= regs[ra_a];
    end
    always_ff @(posedge clk)
        regs_q <= regs[rr_a];

    /* {state, level, phase accumulator} by operator offset. The walk holds
     * the slot's address for all 15 clocks, so the registered read is the
     * slot's word from clock 1 on. */
    (* ramstyle = "M10K" *)
    logic [29:0] slot_mem[32];
    logic [4:0] sl_ra, sl_wa, sl_a, sl_w;
    logic [29:0] sl_q, sl_wd;
    logic sl_we;
    always_comb begin
        sl_a = sst_own ? sst_addr[4:0] : sl_ra;
        sl_w = sst_own ? sst_addr[4:0] : sl_wa;
    end
    always_ff @(posedge clk_a)
        if (sst_own ? sst_we && sel_s : sl_we)
            slot_mem[sl_w] <= sst_own ? sst_wdata[29:0] : sl_wd;
    always_ff @(posedge clk_a)
        sl_q <= slot_mem[sl_a];

    /* A modulator's last two outputs, {h2, h1}, by {slot / 6, slot % 3}. */
    (* ramstyle = "M10K" *)
    logic [25:0] chan_mem[16];
    logic [3:0] ch_ra, ch_wa, cm_a, cm_w;
    logic [25:0] ch_q, ch_wd;
    logic ch_we;
    always_comb begin
        cm_a = sst_own ? sst_addr[3:0] : ch_ra;
        cm_w = sst_own ? sst_addr[3:0] : ch_wa;
    end
    always_ff @(posedge clk_a)
        if (sst_own ? sst_we && sel_c : ch_we)
            chan_mem[cm_w] <= sst_own ? sst_wdata[25:0] : ch_wd;
    always_ff @(posedge clk_a)
        ch_q <= chan_mem[cm_a];

    /* log-sin at 0, (exp | 1024) << 1 at 256 (opl.md 4.1). */
    (* ramstyle = "M10K, no_rw_check" *)
    logic [11:0] rom[512];
    logic [8:0] rom_a;
    logic [11:0] rom_q;
    always_ff @(posedge clk)
        rom_q <= rom[rom_a];

    initial begin
        for (int i = 0; i < 256; i++)
            regs[i] = '0;
        for (int i = 0; i < 32; i++)
            slot_mem[i] = SLOT_RESET;
        for (int i = 0; i < 16; i++)
            chan_mem[i] = '0;
        sl_q = '0;
        ch_q = '0;
        for (int i = 0; i < 512; i++)
            rom[i] = OPL_ROM[i];
        regs_q = '0;
        q_rd = '0;
        rom_q = '0;
    end

    /* ---------------- chip state ---------------- */

    logic test3, wse, test7, nts;
    logic dam, dvb, rhy;
    logic [4:0] drums;
    /* c5, c3 of the cymbal and h8, h7, h3, h2 of the hi-hat (opl.md 5.2). */
    logic [1:0] creg;
    logic [3:0] hreg;
    logic [9:0] lfo;
    logic [2:0] vib;
    logic [8:0] tv;
    logic tdir, tof, tstep;
    logic [17:0] egt;
    logic egc, sub;
    logic [3:0] egshift;
    logic [1:0] eglow;
    logic [22:0] noise;
    /* Slot 15 weighted, slots 16 and 17 raw, from the last sample. */
    logic signed [14:0] held15, held1617;
    logic signed [17:0] acc;
    /* In 24 master clocks, 0..8. */
    logic [3:0] credit /*verilator public_flat_rd*/;
    /* Copies the next frame makes: bits 0-2 the channel 0-2 modulator
     * views, 3-5 the FB of C3-C5. */
    logic [5:0] lag;
    /* This frame makes channel 0-2 copies, so it pops nothing. */
    logic late /*verilator public_flat_rd*/;
    /* A pointer write resets the engine at the next pop, and a parked
     * chip takes no writes, so it outputs 0. */
    logic rst_pend /*verilator public_flat_rd*/;
    /* This sample's AM depth and noise bits. */
    logic [4:0] amv;
    logic hhn, sdn;

    /* ---------------- the frame ---------------- */

    /* A frame each tick: the view copies, the pop and its register-file
     * writes, the tremolo, 18 slots of 15 clocks, then the counters and the
     * output. A pointer write before the pop turns it into the reset, and
     * the pop follows the clear. */
    typedef enum logic [3:0] {
        F_IDLE, F_CLEAR, F_LAG, F_POP, F_APPLY, F_APPLY2, F_PREP, F_WALK,
        F_POST
    } fstate_t;
    fstate_t fstate /*verilator public_flat_rd*/;
    logic [7:0] cnt;

    logic [TICK_W-1:0] tickctr /*verilator public_flat_rd*/;
    logic tick;
    always_comb tick = tickctr == TICK_W'(TICKS_PER_SAMPLE - 1) && !sst_park;

    /* The walk's slot: s = 6 * grp + m6. */
    logic [1:0] grp;
    logic [2:0] m6;
    logic is_mod, rgrp, s13, s14, s15, s16, s17;
    logic [1:0] cm;
    logic [3:0] cn, mv_ab, mv_c;
    logic [4:0] off;
    always_comb begin
        is_mod = m6 < 3'd3;
        cm = is_mod ? m6[1:0] : m6[1:0] - 2'd3;
        cn = {2'd0, grp} + {1'b0, grp, 1'b0} + {2'd0, cm};
        off = {grp, m6};
        rgrp = grp == 2'd2;
        s13 = rgrp && m6 == 3'd1;
        s14 = rgrp && m6 == 3'd2;
        s15 = rgrp && m6 == 3'd3;
        s16 = rgrp && m6 == 3'd4;
        s17 = rgrp && m6 == 3'd5;
        mv_ab = cn + ((is_mod && grp == 2'd0) ? 4'd9 : 4'd0);
        mv_c = cn + (rgrp ? 4'd0 : 4'd9);
    end

    /* The slot's registers, read in walk steps 0-8. */
    logic [7:0] w20, w40, w60, w80, wa;
    logic [5:0] wb;
    logic [3:0] wc;
    logic [1:0] wws;
    logic [18:0] w_p;
    logic [8:0] w_lv;
    logic [1:0] w_st;
    logic signed [12:0] w_h1, w_h2;
    logic [9:0] fnum;
    logic [2:0] blk;
    always_comb begin
        fnum = {wb[1:0], wa};
        blk = wb[4:2];
        w_p = sl_q[18:0];
        w_lv = sl_q[27:19];
        w_st = sl_q[29:28];
        w_h1 = $signed(ch_q[12:0]);
        w_h2 = $signed(ch_q[25:13]);
    end

    /* Step 9: key, rate, KSL and vibrato (opl.md 2, 3.2, 3.5). */
    logic e1_drum, e1_key, e1_dokon, e1_ns;
    logic [1:0] e1_rsel;
    logic [3:0] e1_r, e1_ks, e1_rhi;
    logic [4:0] e1_rsum;
    logic [7:0] e1_kt;
    logic [5:0] e1_k6;
    logic [2:0] e1_vr;
    logic [10:0] e1_f;
    always_comb begin
        case (m6)
            3'd0, 3'd3: e1_drum = drums[4];
            3'd1: e1_drum = drums[0];
            3'd2: e1_drum = drums[2];
            3'd4: e1_drum = drums[3];
            default: e1_drum = drums[1];
        endcase
        e1_key = wb[5] || (rgrp && rhy && e1_drum);
        e1_dokon = w_st == EG_REL && e1_key;
        e1_rsel = e1_dokon ? EG_ATT : w_st;
        case (e1_rsel)
            EG_ATT: e1_r = w60[7:4];
            EG_DEC: e1_r = w60[3:0];
            EG_SUS: e1_r = w20[5] ? 4'd0 : w80[3:0];
            default: e1_r = w80[3:0];
        endcase
        e1_ns = nts ? fnum[8] : fnum[9];
        e1_ks = w20[4] ? {blk, e1_ns} : {2'd0, blk[2:1]};
        e1_rsum = {1'b0, e1_r} + {3'd0, e1_ks[3:2]};
        e1_rhi = e1_rsum[4] ? 4'd15 : e1_rsum[3:0];
        e1_kt = {1'b0, KSL_ROM[fnum[9:6]]} + {2'd0, blk, 3'd0};
        e1_k6 = e1_kt < 8'd64 ? 6'd0 : e1_kt[5:0];
        e1_vr = 3'd0;
        if (w20[6]) begin
            if (vib[1:0] == 2'd2)
                e1_vr = dvb ? fnum[9:7] : {1'b0, fnum[9:8]};
            else if (vib[0])
                e1_vr = dvb ? {1'b0, fnum[9:8]} : {2'd0, fnum[9]};
        end
        e1_f = vib[2] ? {1'b0, fnum - {7'd0, e1_vr}}
                      : {1'b0, fnum} + {8'd0, e1_vr};
    end
    logic e_key, e_dokon, e_rnz;
    logic [3:0] e_rhi;
    logic [1:0] e_rlo;
    logic [5:0] e_k6;
    logic [10:0] e_f;

    /* Step 10: envelope step and state, attenuation, phase increment and
     * feedback (opl.md 2, 3.3-3.5, 4.3). */
    logic [4:0] e2_x, e2_slv;
    logic e2_h, e2_st1, e2_st2, e2_st3;
    logic e2_slreach, e2_zero, e2_silent, e2_linear, e2_expo, e2_mute;
    logic [1:0] e2_next;
    logic [8:0] e2_base, e2_lv, e2_eg;
    logic signed [9:0] e2_add;
    logic [2:0] e2_m;
    (* multstyle = "dsp" *)
    logic signed [12:0] e2_lvs;
    logic [7:0] e2_kslv;
    logic [4:0] e2_am;
    logic [9:0] e2_tot;
    logic [10:0] e2_fx;
    logic [11:0] e2_mb;
    (* multstyle = "dsp" *)
    logic [20:0] e2_ph;
    logic signed [13:0] e2_fbsum;
    logic [6:0] e2_fbm;
    (* multstyle = "dsp" *)
    logic signed [21:0] e2_fbp;
    logic [9:0] e2_fbmod;
    always_comb begin
        e2_x = {1'b0, e_rhi} + {1'b0, egshift};
        case (e_rlo)
            2'd0: e2_h = 1'b0;
            2'd1: e2_h = eglow == 2'd0;
            2'd2: e2_h = !eglow[0];
            default: e2_h = eglow != 2'd3;
        endcase
        e2_st1 = 1'b0;
        e2_st2 = 1'b0;
        e2_st3 = 1'b0;
        if (e_rhi < 4'd12)
            e2_st1 = e_rnz && sub && (e2_x == 5'd12
                || (e2_x == 5'd13 && e_rlo[1]) || (e2_x == 5'd14 && e_rlo[0]));
        else
            case (e_rhi[1:0])
                2'd0: e2_st1 = e2_h || sub;
                2'd1: begin
                    e2_st2 = e2_h;
                    e2_st1 = !e2_h;
                end
                2'd2: begin
                    e2_st3 = e2_h;
                    e2_st2 = !e2_h;
                end
                default: e2_st3 = 1'b1;
            endcase

        e2_slv = w80[7:4] == 4'd15 ? 5'd31 : {1'b0, w80[7:4]};
        e2_slreach = w_lv[8:4] == e2_slv;
        e2_zero = w_lv == 9'd0;
        e2_silent = &w_lv[8:3];
        if (e_dokon)
            e2_next = EG_ATT;
        else if (!e_key)
            e2_next = EG_REL;
        else if (w_st == EG_ATT)
            e2_next = e2_zero ? EG_DEC : EG_ATT;
        else if (w_st == EG_DEC)
            e2_next = e2_slreach ? EG_SUS : EG_DEC;
        else
            e2_next = w_st;
        e2_linear = !e_dokon && !e2_silent && (w_st == EG_SUS
            || w_st == EG_REL || (w_st == EG_DEC && !e2_slreach));
        e2_expo = w_st == EG_ATT && e_key && e_rhi != 4'd15 && !e2_zero;
        e2_mute = w_st != EG_ATT && e2_silent && !e_dokon;
        e2_base = e2_mute ? 9'h1FF : (e_dokon && e_rhi == 4'd15) ? 9'd0 : w_lv;
        e2_add = e2_expo ? $signed(~{1'b0, w_lv}) >>> 1
               : e2_linear ? 10'sd4 : 10'sd0;
        e2_m = {e2_st3, e2_st2, e2_st1};
        e2_lvs = $signed({2'b00, e2_base, 2'b00})
            + 13'(e2_add) * $signed({10'd0, e2_m});
        e2_lv = e2_lvs[10:2];

        case (w40[7:6])
            2'd0: e2_kslv = 8'd0;
            2'd1: e2_kslv = {1'b0, e_k6, 1'b0};
            2'd2: e2_kslv = {2'd0, e_k6};
            default: e2_kslv = {e_k6, 2'd0};
        endcase
        e2_am = w20[7] ? amv : 5'd0;
        e2_tot = {1'b0, w_lv} + {2'd0, e2_kslv} + {2'd0, w40[5:0], 2'd0}
            + {5'd0, e2_am};
        e2_eg = e2_tot[9] ? 9'h1FF : e2_tot[8:0];

        /* The block shift moves onto MULT, and the phase add rides the
         * product: (f << blk) / 2 * mt, over 2, is x * (mt << blk) / 4. */
        e2_fx = {e_f[10:1], e_f[0] && blk != 3'd0};
        e2_mb = {7'd0, MT[w20[3:0]]} << blk;
        e2_ph = {(e_dokon ? 19'd0 : w_p), 2'b00}
            + 21'(e2_fx) * 21'(e2_mb);

        /* >>> (9 - fb) is * 2^(fb - 1) >>> 8. */
        e2_fbsum = 14'(w_h1) + 14'(w_h2);
        e2_fbm = wc[3:1] == 3'd0 ? 7'd0 : 7'd1 << (wc[3:1] - 3'd1);
        e2_fbp = 22'(e2_fbsum) * $signed({15'd0, e2_fbm});
        e2_fbmod = e2_fbp[17:8];
    end
    logic [8:0] e_lv, e_eg;
    logic [1:0] e_st;
    logic e_mute;
    logic [9:0] e_fbmod;

    /* Step 11: the phase this sample, rhythm substitution, modulation and
     * the log-sin address (opl.md 4.2, 4.3, 5.2). */
    logic [9:0] e3_phase, e3_ph, e3_mod, e3_p;
    logic e3_h2, e3_h3, e3_h7, e3_h8, e3_c3, e3_c5, e3_rm;
    logic [7:0] e3_idx;
    always_comb begin
        e3_phase = w_p[18:9];
        e3_h2 = s13 ? e3_phase[2] : hreg[0];
        e3_h3 = s13 ? e3_phase[3] : hreg[1];
        e3_h7 = s13 ? e3_phase[7] : hreg[2];
        e3_h8 = s13 ? e3_phase[8] : hreg[3];
        e3_c3 = s17 ? e3_phase[3] : creg[0];
        e3_c5 = s17 ? e3_phase[5] : creg[1];
        e3_rm = (e3_h2 ^ e3_h7) | (e3_h3 ^ e3_c5) | (e3_c3 ^ e3_c5);
        if (rhy && s13)
            e3_ph = {e3_rm, (e3_rm ^ hhn) ? 9'h0D0 : 9'h034};
        else if (rhy && s16)
            e3_ph = {e3_h8, e3_h8 ^ sdn, 8'd0};
        else if (rhy && s17)
            e3_ph = {e3_rm, 9'h100};
        else
            e3_ph = e3_phase;
        if (is_mod)
            e3_mod = (rhy && (s13 || s14)) ? 10'd0 : e_fbmod;
        else
            e3_mod = (wc[0] || (rhy && (s16 || s17))) ? 10'd0 : w_h1[9:0];
        e3_p = e3_ph + e3_mod;
        e3_idx = e3_p[8] ? ~e3_p[7:0] : e3_p[7:0];
    end
    logic [18:0] e_pn;
    logic e_p9, e_p8;

    /* Step 12: attenuation into the exp address. */
    logic [12:0] e4_att;
    logic [11:0] e4_attc;
    always_comb begin
        e4_att = {1'b0, rom_q} + {1'b0, e_eg, 3'd0};
        e4_attc = e4_att[12] ? 12'hFFF : e4_att[11:0];
    end
    logic [3:0] e_ash;

    /* Step 13: the shift and the waveform. */
    logic [11:0] e5_v, e5_m;
    (* multstyle = "dsp" *)
    logic [23:0] e5_p;
    logic signed [12:0] e5_pos, e5_out;
    always_comb begin
        e5_m = e_ash > 4'd11 ? 12'd0 : 12'd1 << (4'd11 - e_ash);
        e5_p = 24'(rom_q) * 24'(e5_m);
        e5_v = e5_p[22:11];
        e5_pos = $signed({1'b0, e5_v});
        case (wws)
            2'd0: e5_out = e_p9 ? ~e5_pos : e5_pos;
            2'd1: e5_out = e_p9 ? 13'sd0 : e5_pos;
            2'd2: e5_out = e5_pos;
            default: e5_out = e_p8 ? 13'sd0 : e5_pos;
        endcase
    end
    logic signed [12:0] e_out;

    /* Step 14: the mix (opl.md 6.1). */
    logic e6_incl, e6_hist;
    logic signed [14:0] e6_raw, e6_add;
    always_comb begin
        if (is_mod)
            e6_incl = (rhy && rgrp) ? (s13 || s14) : wc[0];
        else
            e6_incl = 1'b1;
        e6_raw = e_mute ? 15'sd0 : 15'(e_out);
        e6_add = !e6_incl ? 15'sd0 : (rhy && rgrp) ? e6_raw <<< 1 : e6_raw;
        e6_hist = is_mod && !(rhy && (s13 || s14));
    end

    /* YM3014B (opl.md 6.2). */
    logic signed [15:0] dac_s, dac;
    logic [5:0] dac_t;
    logic [3:0] dac_e;
    always_comb begin
        if (acc > 18'sd32767)
            dac_s = 16'sd32767;
        else if (acc < -18'sd32768)
            dac_s = 16'sh8000;
        else
            dac_s = acc[15:0];
        dac_t = dac_s[15] ? ~dac_s[14:9] : dac_s[14:9];
        dac_e = dac_t[5] ? 4'd6 : dac_t[4] ? 4'd5 : dac_t[3] ? 4'd4
              : dac_t[2] ? 4'd3 : dac_t[1] ? 4'd2 : dac_t[0] ? 4'd1 : 4'd0;
        dac = dac_s & $signed(16'hFFFF << dac_e);
    end

    /* EG timer (opl.md 1.3). */
    logic [18:0] egt_n;
    logic [3:0] egs_n;
    always_comb begin
        egt_n = {1'b0, egt} + {18'd0, egc | sub};
        egs_n = 4'd0;
        for (int i = 12; i >= 0; i--)
            if (egt_n[i])
                egs_n = 4'(i + 1);
    end

    /* Tremolo (opl.md 1.3). */
    logic [6:0] tout;
    logic t_of, t_dir;
    always_comb begin
        tout = tv[6:0];
        t_of = tout == 7'd0 || (tout & 7'd105) == 7'd105;
        t_dir = (t_of && !tof) ? !tdir : tdir;
    end

    /* The lag copies, cv to mv: A0-A2, B0-B2, C0-C5. */
    function automatic logic [3:0] lag_src(logic [3:0] k);
        if (k < 4'd3)
            lag_src = k;
        else if (k < 4'd6)
            lag_src = k - 4'd3;
        else
            lag_src = k - 4'd6;
    endfunction
    function automatic logic [3:0] lag_grp(logic [3:0] k);
        lag_grp = k < 4'd3 ? 4'hA : k < 4'd6 ? 4'hB : 4'hC;
    endfunction
    logic [3:0] lk_r, lk_w, lc_w;
    always_comb begin
        lk_r = cnt[3:0];
        lk_w = cnt[3:0] - 4'd1;
        lc_w = lag_src(lk_w);
    end

    /* The write the queue pops: decoded, and its two register-file
     * writes. */
    logic [7:0] ap_a, ap_d, ap_cv, ap_mv, ap_a1;
    logic [3:0] ap_lo, ap_c;
    logic ap_glob, ap_ch, ap_alias, ap_c35, ap_wr1, ap_wr2, ap_lag;
    always_comb begin
        ap_a = q_rd[7:0];
        ap_d = q_rd[15:8];
        ap_glob = ap_a[7:5] == 3'd0 || ap_a == 8'hBD;
        ap_ch = !ap_glob && (ap_a[7:4] == 4'hA || ap_a[7:4] == 4'hB
            || ap_a[7:4] == 4'hC);
        ap_lo = ap_a[3:0];
        ap_c = ap_lo >= 4'd14 ? ap_lo - 4'd14
             : ap_lo >= 4'd9 ? ap_lo - 4'd9 : ap_lo;
        ap_alias = ap_lo >= 4'd9 && ap_lo <= 4'd11;
        ap_cv = {ap_a[7:4], ap_c};
        ap_mv = {ap_a[7:4], ap_c + 4'd9};
        ap_c35 = ap_a[7:4] == 4'hC && ap_c >= 4'd3 && ap_c <= 4'd5;
        ap_wr1 = !ap_glob && (ap_a[7:5] != 3'd7 || wse);
        ap_wr2 = ap_ch && (ap_alias || ap_c35);
        ap_lag = ap_ch && !ap_alias && (ap_c < 4'd3 || ap_c35);
        ap_a1 = ap_ch ? ap_cv : ap_a;
    end
    logic [7:0] ap2_a, ap2_d;
    logic ap2_rmw;

    logic pop, pop_left;
    logic [3:0] credit_a, credit_cap;
    always_comb begin
        pop = !rst_pend && !late && q_count != 9'd0 && credit >= 4'd4;
        q_take = fstate == F_POP && pop;
        pop_left = pop ? q_count > 9'd1 : q_count != 9'd0;
        credit_a = (pop ? credit - 4'd4 : credit) + 4'd3;
        credit_cap = pop_left ? 4'd8 : 4'd4;
    end

    /* Memory ports by frame step. */
    always_comb begin
        rr_a = {4'hC, mv_c};
        rw_we = 1'b0;
        rw_a = cnt;
        rw_d = 8'd0;
        sl_ra = 5'd0;
        sl_we = 1'b0;
        sl_wa = off;
        sl_wd = {e_st, e_lv, e_pn};
        ch_ra = {grp, cm};
        ch_we = 1'b0;
        ch_wa = {grp, cm};
        ch_wd = {w_h1, e_out};
        rom_a = {1'b0, e3_idx};
        case (fstate)
            F_CLEAR: begin
                rw_we = 1'b1;
                sl_we = cnt[7:5] == 3'd0;
                sl_wa = cnt[4:0];
                sl_wd = SLOT_RESET;
                ch_we = cnt[7:4] == 4'd0;
                ch_wa = cnt[3:0];
                ch_wd = '0;
            end
            F_LAG: begin
                rr_a = {lag_grp(lk_r), lag_src(lk_r)};
                rw_we = cnt != 8'd0 && lag[lc_w[2:0]];
                rw_a = {lag_grp(lk_w), lc_w + 4'd9};
                rw_d = regs_q;
            end
            F_APPLY: begin
                rr_a = ap_mv;
                rw_we = ap_wr1;
                rw_a = ap_a1;
                rw_d = ap_d;
            end
            F_APPLY2: begin
                rw_we = 1'b1;
                rw_a = ap2_a;
                rw_d = ap2_rmw ? {regs_q[7:1], ap2_d[0]} : ap2_d;
            end
            F_WALK: begin
                sl_ra = off;
                case (cnt[3:0])
                    4'd0: rr_a = {3'd1, off};
                    4'd1: rr_a = {3'd2, off};
                    4'd2: rr_a = {3'd3, off};
                    4'd3: rr_a = {3'd4, off};
                    4'd4: rr_a = {3'd7, off};
                    4'd5: rr_a = {4'hA, mv_ab};
                    4'd6: rr_a = {4'hB, mv_ab};
                    default: ;
                endcase
                if (cnt[3:0] == 4'd12)
                    rom_a = {1'b1, e4_attc[7:0]};
                if (cnt[3:0] == 4'd14) begin
                    sl_we = 1'b1;
                    ch_we = e6_hist;
                end
            end
            default: ;
        endcase
    end

    initial begin
        test3 = 1'b0;
        wse = 1'b0;
        test7 = 1'b0;
        nts = 1'b0;
        dam = 1'b0;
        dvb = 1'b0;
        rhy = 1'b0;
        drums = '0;
        creg = '0;
        hreg = '0;
        lfo = '0;
        vib = '0;
        tv = '0;
        tdir = 1'b0;
        tof = 1'b1;
        tstep = 1'b0;
        egt = '0;
        egc = 1'b0;
        sub = 1'b0;
        egshift = '0;
        eglow = '0;
        noise = 23'd1;
        held15 = '0;
        held1617 = '0;
        acc = '0;
        credit = 4'd4;
        lag = '0;
        late = 1'b0;
        rst_pend = 1'b0;
        amv = '0;
        hhn = 1'b0;
        sdn = 1'b0;
        fstate = F_IDLE;
        cnt = '0;
        tickctr = '0;
        grp = '0;
        m6 = '0;
        w20 = '0;
        w40 = '0;
        w60 = '0;
        w80 = '0;
        wa = '0;
        wb = '0;
        wc = '0;
        wws = '0;
        e_key = 1'b0;
        e_dokon = 1'b0;
        e_rnz = 1'b0;
        e_rhi = '0;
        e_rlo = '0;
        e_k6 = '0;
        e_f = '0;
        e_lv = '0;
        e_eg = '0;
        e_st = '0;
        e_mute = 1'b0;
        e_fbmod = '0;
        e_pn = '0;
        e_p9 = 1'b0;
        e_p8 = 1'b0;
        e_ash = '0;
        e_out = '0;
        ap2_a = '0;
        ap2_d = '0;
        ap2_rmw = 1'b0;
        opl_out = '0;
        opl_valid = 1'b0;
    end
    always_ff @(posedge clk) begin
        opl_valid <= 1'b0;
        if (!sst_park)
            tickctr <= tick ? '0 : tickctr + TICK_W'(1);

        /* A frame that outlasts its tick loses a sample. */
        if (tick && fstate != F_IDLE)
            $fatal(1, "opl walk overrun");

        case (fstate)
            F_IDLE: if (tick) begin
                cnt <= '0;
                fstate <= F_LAG;
            end

            F_LAG: begin
                if (cnt == 8'd0)
                    late <= |lag[2:0];
                cnt <= cnt + 8'd1;
                if (cnt == 8'd12) begin
                    lag <= '0;
                    fstate <= F_POP;
                end
            end

            F_POP: if (rst_pend) begin
                /* opl_reset: the queue went with the pointer write. */
                rst_pend <= 1'b0;
                test3 <= 1'b0;
                wse <= 1'b0;
                test7 <= 1'b0;
                nts <= 1'b0;
                dam <= 1'b0;
                dvb <= 1'b0;
                rhy <= 1'b0;
                drums <= '0;
                creg <= '0;
                lfo <= '0;
                vib <= '0;
                tv <= '0;
                tdir <= 1'b0;
                tof <= 1'b1;
                tstep <= 1'b0;
                egt <= '0;
                egc <= 1'b0;
                sub <= 1'b0;
                egshift <= '0;
                eglow <= '0;
                noise <= 23'd1;
                held15 <= '0;
                held1617 <= '0;
                credit <= 4'd4;
                late <= 1'b0;
                cnt <= '0;
                fstate <= F_CLEAR;
            end else begin
                credit <= credit_a > credit_cap ? credit_cap : credit_a;
                fstate <= pop ? F_APPLY : F_PREP;
            end

            F_CLEAR: begin
                cnt <= cnt + 8'd1;
                if (cnt == 8'hFF)
                    fstate <= F_POP;
            end

            F_APPLY: begin
                if (ap_a == 8'h01) begin
                    test3 <= ap_d[3];
                    wse <= ap_d[5];
                    test7 <= ap_d[7];
                end
                if (ap_a == 8'h08)
                    nts <= ap_d[6];
                if (ap_a == 8'hBD) begin
                    /* Turning rhythm on latches the cymbal bits from slot
                     * 0's phase (opl.md 5.4). */
                    if (!rhy && ap_d[5])
                        creg <= {sl_q[14], sl_q[12]};
                    dam <= ap_d[7];
                    dvb <= ap_d[6];
                    rhy <= ap_d[5];
                    drums <= ap_d[4:0];
                end
                if (ap_lag)
                    lag[ap_c[2:0]] <= 1'b1;
                ap2_a <= ap_mv;
                ap2_d <= ap_d;
                ap2_rmw <= ap_c35;
                fstate <= ap_wr2 ? F_APPLY2 : F_PREP;
            end

            F_APPLY2: fstate <= F_PREP;

            F_PREP: begin
                tof <= t_of;
                if (test7) begin
                    tv <= '0;
                    tdir <= 1'b0;
                end else begin
                    tdir <= t_dir;
                    if (tstep || test3)
                        tv <= t_dir ? tv - 9'd1 : tv + 9'd1;
                end
                amv <= dam ? tout[6:2] : {2'd0, tout[6:4]};
                hhn <= noise[9];
                sdn <= noise[6];
                acc <= 18'(held15) + (rhy ? 18'(held1617) <<< 1
                                          : 18'(held1617));
                cnt <= '0;
                grp <= '0;
                m6 <= '0;
                fstate <= F_WALK;
            end

            F_WALK: begin
                case (cnt[3:0])
                    4'd1: w20 <= regs_q;
                    4'd2: w40 <= regs_q;
                    4'd3: w60 <= regs_q;
                    4'd4: w80 <= regs_q;
                    4'd5: wws <= regs_q[1:0];
                    4'd6: wa <= regs_q;
                    4'd7: wb <= regs_q[5:0];
                    4'd8: wc <= regs_q[3:0];
                    4'd9: begin
                        e_key <= e1_key;
                        e_dokon <= e1_dokon;
                        e_rnz <= e1_r != 4'd0;
                        e_rhi <= e1_rhi;
                        e_rlo <= e1_ks[1:0];
                        e_k6 <= e1_k6;
                        e_f <= e1_f;
                    end
                    4'd10: begin
                        e_lv <= e2_lv;
                        e_st <= e2_next;
                        e_mute <= e2_mute;
                        e_eg <= e2_eg;
                        e_pn <= e2_ph[20:2];
                        e_fbmod <= e2_fbmod;
                    end
                    4'd11: begin
                        e_p9 <= e3_p[9];
                        e_p8 <= e3_p[8];
                        if (s13)
                            hreg <= {e3_phase[8], e3_phase[7], e3_phase[3],
                                     e3_phase[2]};
                        if (s17 && rhy)
                            creg <= {e3_phase[5], e3_phase[3]};
                    end
                    4'd12: e_ash <= e4_attc[11:8];
                    4'd13: e_out <= e5_out;
                    default: ;
                endcase
                if (cnt[3:0] == 4'd14) begin
                    if (!(rgrp && m6 >= 3'd3))
                        acc <= acc + 18'(e6_add);
                    if (s15)
                        held15 <= e6_add;
                    if (s16)
                        held1617 <= e6_raw;
                    if (s17)
                        held1617 <= held1617 + e6_raw;
                    noise <= {noise[21:0], test7 | (noise[22] ^ noise[8])};
                    /* Test bit 7 holds the vibrato at 0 from slot 1. */
                    if (grp == 2'd0 && m6 == 3'd0 && test7)
                        vib <= '0;
                    cnt <= '0;
                    if (m6 == 3'd5) begin
                        m6 <= '0;
                        grp <= grp + 2'd1;
                        if (rgrp)
                            fstate <= F_POST;
                    end else
                        m6 <= m6 + 3'd1;
                end else
                    cnt <= cnt + 8'd1;
            end

            F_POST: begin
                tstep <= !test7 && lfo[5:0] == 6'h3F;
                vib <= test7 ? 3'd0
                    : vib + {2'd0, lfo == 10'h3FF || test3};
                lfo <= test7 ? 10'd0 : lfo + 10'd1;
                egc <= egt_n[18];
                egt <= egt_n[17:0];
                if (sub) begin
                    eglow <= egt_n[1:0];
                    egshift <= egs_n;
                end
                sub <= !sub;
                opl_out <= dac;
                opl_valid <= 1'b1;
                fstate <= F_IDLE;
            end

            default: fstate <= F_IDLE;
        endcase

        /* A pointer write during the reset joins it: nothing has changed
         * since the reset began. */
        if (xaddr_we && fstate != F_CLEAR && !(fstate == F_POP && rst_pend))
            rst_pend <= 1'b1;

        if (sst_st_we && sel_st)
            case (st_k)
                3'd0: {rst_pend, credit} <= sst_wdata[29:25];
                3'd2: {tdir, tof, tstep, creg, drums, rhy, dvb, dam, nts,
                       test7, wse, test3, lag} <= sst_wdata[31:9];
                3'd3: begin
                    {tv, vib, lfo} <= sst_wdata[21:0];
                    tickctr <= TICK_W'(sst_wdata[31:22]);
                end
                3'd4: {eglow, egshift, sub, egc, egt} <= sst_wdata[25:0];
                3'd5: noise <= sst_wdata[22:0];
                3'd6: {held1617, held15} <= sst_wdata[29:0];
                default: ;
            endcase
    end

    always_comb
        opl_quiet = fstate == F_IDLE && !opl_valid
            && tickctr < TICK_W'(TICKS_PER_SAMPLE - 1 - QUIET_LEAD);

    logic [31:0] st_rd;
    always_comb begin
        case (st_k)
            3'd0: st_rd = {2'd0, rst_pend, credit, enabled, q_tail, q_head,
                           xaddr[15:8]};
            3'd1: st_rd = {1'b0, opl_drops};
            3'd2: st_rd = {tdir, tof, tstep, creg, drums, rhy, dvb, dam, nts,
                           test7, wse, test3, lag, q_count};
            3'd3: st_rd = {10'(tickctr), tv, vib, lfo};
            3'd4: st_rd = {6'd0, eglow, egshift, sub, egc, egt};
            3'd5: st_rd = {9'd0, noise};
            3'd6: st_rd = {2'd0, held1617, held15};
            /* The start's low byte, zero in a blob from when it started a
             * page. */
            3'd7: st_rd = {24'd0, xaddr[7:0]};
            default: st_rd = '0;
        endcase
        opl_sst_rdata = sel_q ? {16'd0, qa_q}
            : sel_r ? {24'd0, ra_q}
            : sel_s ? {2'd0, sl_q}
            : sel_c ? {6'd0, ch_q}
            : sel_st ? st_rd : '0;
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_opl;
    always_comb unused_opl = ^{clk_sys, e2_ph[1:0], e2_fbp[21:18],
                               e2_fbp[7:0], e2_lvs[12:11], e2_lvs[1:0],
                               e5_p[23], e5_p[10:0], e1_kt[7:6]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
