/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * W65C02S, per-cycle bus-identical to the emulator's chips/w65c02.h. The
 * decode table in w65c02_rom_pkg comes from the same generator as the C, and
 * the conformance suites in tests/wdc hold both to the same vectors.
 *
 * One clock per CPU cycle when en is high. Outputs are registered and describe
 * the bus cycle in flight: the memory system returns data_i for the address on
 * w65c02_addr in the same en period, exactly the contract of w65c02_tick.
 *
 * Statement order in the C decides evaluation order here: operations run
 * first, the address unit uses post-operation values, and post-steps land
 * last. RTS drives the program counter it just pulled.
 */

module w65c02
    import w65c02_rom_pkg::*;
(
    input logic clk,
    input logic rst_n,
    input logic en,

    input logic [7:0] data_i,
    input logic irq_i,
    input logic nmi_i,
    input logic rdy_i,
    input logic res_i,

    output logic [15:0] w65c02_addr,
    output logic [7:0] w65c02_data,
    output logic w65c02_we,
    output logic w65c02_stp,
    /* Beside stp because a freeze has to know about both: neither stall
     * cycle fetches, so a gate that waits for sync alone never lets go
     * of a core sitting in WAI. */

    /* Everything the core is, as five words, for stopping it in one
     * place and starting it again somewhere else. Reads are free; a
     * write lands directly in the flops, which is only sound while en
     * is held low, and that is the caller's part of the bargain.
     *
     * The registers are the obvious half. The half that is not: the
     * interrupt pipelines, because a detection is honoured two
     * boundaries after it arrives and a core restored without them
     * takes the interrupt late or never; and the wait and stop flags,
     * because a core in WAI has to come back still in WAI.
     *
     * ad, ir, tick, the brk causes and res_seen are dead at an
     * instruction boundary, which is the only place a freeze lands.
     * They travel anyway — they cost nothing in words already being
     * carried, and the port then describes the core rather than one
     * moment in it. */
    input logic [2:0] st_idx,
    output logic [31:0] w65c02_st_rdata,
    /* The whole of it at once, because a restore lands with the clock
     * already back. */
    input logic st_jam,
    input logic [31:0] st_jam_data[5],

    /* What the bus will carry after this enable — the same values the
     * registers take at the edge, published before it. A memory that
     * cannot answer inside one clock has to launch on the enable rather
     * than a clock later, or its byte arrives with nothing left of the
     * period for the address cone to consume it. */
    output logic [15:0] w65c02_next_addr,
    output logic [7:0] w65c02_next_data,
    output logic w65c02_next_we
);

    /* Fetch cycle marker. Once an output for the freeze boundary; the
     * clock-stop design needs no boundary, so it is internal state
     * again -- the interrupt takes and the blob still ride on it. */
    logic w65c02_sync /*verilator public_flat_rd*/;

    // P flag bits
    localparam int FC = 0;
    localparam int FZ = 1;
    localparam int FI = 2;
    localparam int FD = 3;
    localparam int FB = 4;
    localparam int FX = 5;
    localparam int FV = 6;
    localparam int FN = 7;

    // Public for the conformance testbench, which adopts register state to
    // start a vector mid-stream the way the C model can.
    logic [15:0] pc /*verilator public_flat_rw*/;
    logic [15:0] ad /*verilator public_flat_rw*/;
    logic [7:0] a /*verilator public_flat_rw*/;
    logic [7:0] x /*verilator public_flat_rw*/;
    logic [7:0] y /*verilator public_flat_rw*/;
    logic [7:0] s /*verilator public_flat_rw*/;
    logic [7:0] p /*verilator public_flat_rw*/;
    logic [7:0] ir /*verilator public_flat_rw*/;
    logic [2:0] tick /*verilator public_flat_rw*/;
    // Interrupt pipelines: detections enter at bit 8 and shift up each cycle,
    // so a request is honored at the second SYNC after it, like the silicon.
    logic [15:0] irq_pip /*verilator public_flat_rw*/;
    logic [15:0] nmi_pip /*verilator public_flat_rw*/;
    logic brk_irq /*verilator public_flat_rw*/;
    logic brk_nmi /*verilator public_flat_rw*/;
    logic brk_res /*verilator public_flat_rw*/;
    logic wait_flag /*verilator public_flat_rw*/;
    logic stop_flag /*verilator public_flat_rw*/;
    always_comb w65c02_stp = stop_flag;
    logic nmi_prev /*verilator public_flat_rw*/;
    // The power-on one-shot only: the C model comes out of init with RES
    // pending for its first SYNC. At runtime RES is a level sampled at SYNC,
    // exactly as the C does it; a pulse between opcode fetches is lost on
    // both sides, and the machine holds RESB far longer than an instruction.
    logic res_seen /*verilator public_flat_rw*/;

    // ------------------------------------------------------------------
    // Stalls, detection, and the SYNC prologue
    // ------------------------------------------------------------------


    /* The five words. Packed so a byte register never straddles one:
     * the engine writes them back verbatim and nothing has to agree
     * about bit order twice. */
    always_comb begin
        case (st_idx)
            3'd0: w65c02_st_rdata = {s, y, x, a};
            3'd1: w65c02_st_rdata = {ir, p, pc};
            3'd2:
            w65c02_st_rdata = {
                ad,
                5'd0,
                tick,
                res_seen,
                nmi_prev,
                stop_flag,
                wait_flag,
                brk_res,
                brk_nmi,
                brk_irq,
                w65c02_we
            };
            3'd3: w65c02_st_rdata = {irq_pip, nmi_pip};
            3'd4: w65c02_st_rdata = {w65c02_addr, w65c02_data, 7'd0, w65c02_sync};
            default: w65c02_st_rdata = 32'd0;
        endcase
    end

    logic stop_stall, wait_stall, rdy_stall;
    always_comb begin
        stop_stall = stop_flag && !res_i;
        wait_stall = wait_flag && !stop_stall && !(irq_i || nmi_i || res_i);
        // RDY freezes read cycles only; the C checks it before decoding.
        rdy_stall = !stop_flag && !wait_flag && rdy_i && !w65c02_we;
    end

    logic [15:0] irq_pip_det, nmi_pip_det;
    always_comb begin
        irq_pip_det = irq_pip;
        if (irq_i && !p[FI])
            irq_pip_det = irq_pip | 16'h0100;
        nmi_pip_det = nmi_pip;
        if (nmi_i && !nmi_prev)
            nmi_pip_det = nmi_pip | 16'h0100;
    end

    logic take_irq, take_nmi, take_res, brk_now;
    logic [7:0] eff_ir;
    logic [2:0] eff_tick;
    logic [15:0] pc_pro;  // after the prologue's PC increment
    always_comb begin
        take_irq = w65c02_sync && irq_pip_det[10];
        take_nmi = w65c02_sync && |nmi_pip_det[15:10];
        take_res = w65c02_sync && (res_seen || res_i);
        brk_now = take_irq || take_nmi || take_res;
        if (w65c02_sync) begin
            eff_ir = brk_now ? 8'h00 : data_i;
            eff_tick = 3'd0;
            pc_pro = brk_now ? pc : pc + 16'd1;
        end else begin
            eff_ir = ir;
            eff_tick = tick;
            pc_pro = pc;
        end
    end

    w65c02_cw_t cw;
    always_comb cw = w65c02_rom({eff_ir, eff_tick});

    // brk_* as the executing BRK sequence sees them: forced entry latches its
    // cause in the same cycle tick 0 runs.
    logic seq_irq, seq_nmi, seq_res;
    always_comb begin
        seq_irq = brk_irq || take_irq;
        seq_nmi = brk_nmi || take_nmi;
        seq_res = brk_res || take_res;
    end

    // ------------------------------------------------------------------
    // Operation execute: post-operation register values
    // ------------------------------------------------------------------

    // N and Z from a result; bits 7 and 1 of the incoming P are replaced.
    /* verilator lint_off UNUSEDSIGNAL */
    function automatic logic [7:0] nz(input logic [7:0] pin, input logic [7:0] v);
        return {v[7], pin[6:2], v == 8'h00, pin[0]};
    endfunction
    /* verilator lint_on UNUSEDSIGNAL */

    // Shifts and rotates share one result-and-carry form.
    function automatic logic [8:0] shift(input w65c02_rom_pkg::w65c02_sel_t which,
                                         input logic [7:0] v,
                                         input logic cin);
        case (which)
            SEL_ASL: return {v[7], v[6:0], 1'b0};
            SEL_LSR: return {v[0], 1'b0, v[7:1]};
            SEL_ROL: return {v[7], v[6:0], cin};
            default: return {v[0], cin, v[7:1]};  // SEL_ROR
        endcase
    endfunction

    logic [15:0] n_pc, n_ad;
    logic [7:0] n_a, n_x, n_y, n_s, n_p;
    logic n_wait, n_stop;
    logic n_brk_irq, n_brk_nmi, n_brk_res;
    logic cond_fail;  // drive the alternate address, suppress fetch and skip
    logic pip_hold;   // a taken same-page branch delays interrupt recognition
    logic wr_gate;    // interrupt entry suppresses the reset variant's pushes

    // Scratch used across the big case
    logic [8:0] sum9;
    logic [15:0] cmp16, bin16;
    logic [7:0] gd, opnd;
    logic [5:0] al6, ah6;
    logic flagbit;
    logic branch_taken;
    logic lo_borrow;
    logic [3:0] bsel;

    always_comb begin
        n_pc = pc_pro;
        n_ad = ad;
        n_a = a;
        n_x = x;
        n_y = y;
        n_s = s;
        n_p = brk_now ? (p & ~(8'h01 << FB)) : p;
        // A set flag only reaches this path on its release cycle, so the
        // default is the C's clear-and-continue.
        n_wait = 1'b0;
        n_stop = 1'b0;
        n_brk_irq = seq_irq;
        n_brk_nmi = seq_nmi;
        n_brk_res = seq_res;
        cond_fail = 1'b0;
        pip_hold = 1'b0;
        wr_gate = 1'b1;
        gd = data_i;
        opnd = 8'h00;
        sum9 = 9'h0;
        cmp16 = 16'h0;
        bin16 = 16'h0;
        al6 = 6'h0;
        ah6 = 6'h0;
        flagbit = 1'b0;
        branch_taken = 1'b0;
        lo_borrow = 1'b0;
        bsel = 4'h0;

        unique case (cw.op)
            OP_NONE: ;

            // ---- moving bytes ----
            OP_AD_LOAD: n_ad = {8'h00, gd};
            OP_AD_HI: n_ad = {gd, ad[7:0]};
            OP_AD_HI_INDEX: begin
                n_ad = {gd, ad[7:0]};
                // Page cross: the low-byte add carries. The index register is
                // whichever the no-cross address uses.
                sum9 = {1'b0, ad[7:0]} + {1'b0, (cw.off == OFF_X) ? x : y};
                cond_fail = sum9[8];  // crossed: dummy read instead, no skip
            end
            OP_AD_ADD_X_ZP: n_ad = {8'h00, ad[7:0] + x};
            OP_AD_ADD_X: n_ad = ad + {8'h00, x};
            OP_LOAD: begin
                case (cw.sel)
                    SEL_X: begin n_x = gd; n_p = nz(n_p, gd); end
                    SEL_Y: begin n_y = gd; n_p = nz(n_p, gd); end
                    default: begin n_a = gd; n_p = nz(n_p, gd); end
                endcase
            end
            OP_PC_FROM_AD: n_pc = ad;
            OP_PC_FROM_ABS: n_pc = {gd, ad[7:0]};
            OP_PCL_FROM_BUS: n_pc = {8'h00, gd};
            OP_PCH_FROM_BUS: n_pc = {gd, pc[7:0]};

            // ---- arithmetic and logic ----
            OP_LOGIC: begin
                case (cw.sel)
                    SEL_AND: n_a = a & gd;
                    SEL_EOR: n_a = a ^ gd;
                    default: n_a = a | gd;
                endcase
                n_p = nz(n_p, n_a);
            end
            OP_CMP: begin
                case (cw.sel)
                    SEL_X: cmp16 = {8'h00, x} - {8'h00, gd};
                    SEL_Y: cmp16 = {8'h00, y} - {8'h00, gd};
                    default: cmp16 = {8'h00, a} - {8'h00, gd};
                endcase
                n_p = nz(n_p, cmp16[7:0]);
                n_p[FC] = ~|cmp16[15:8];
            end
            OP_BIT: begin
                n_p[FZ] = (a & gd) == 8'h00;
                n_p[FN] = gd[7];
                n_p[FV] = gd[6];
            end
            OP_BIT_IMM: n_p[FZ] = (a & gd) == 8'h00;
            OP_ADDSUB_BINARY: begin
                n_ad = {8'h00, gd};
                if (!p[FD]) begin
                    if (cw.sel == SEL_ADC)
                        bin16 = {8'h00, a} + {8'h00, gd} + {15'h0, p[FC]};
                    else
                        bin16 = {8'h00, a} - {8'h00, gd} - {15'h0, ~p[FC]};
                    n_a = bin16[7:0];
                    n_p = nz(n_p, bin16[7:0]);
                    n_p[FV] = (cw.sel == SEL_ADC)
                        ? (~(a[7] ^ gd[7]) & (a[7] ^ bin16[7]))
                        : ((a[7] ^ gd[7]) & (a[7] ^ bin16[7]));
                    n_p[FC] = (cw.sel == SEL_ADC) ? |bin16[15:8] : ~|bin16[15:8];
                end else begin
                    cond_fail = 1'b1;  // decimal: spend the extra cycle
                end
            end
            OP_ADDSUB_DECIMAL: begin
                opnd = ad[7:0];
                if (cw.sel == SEL_ADC) begin
                    al6 = {2'b00, a[3:0]} + {2'b00, opnd[3:0]} + {5'h0, p[FC]};
                    if (al6 > 6'd9)
                        al6 = al6 + 6'd6;
                    ah6 = {2'b00, a[7:4]} + {2'b00, opnd[7:4]} + {5'h0, al6 > 6'h0F};
                    n_p[FV] = ~(a[7] ^ opnd[7]) & (a[7] ^ ah6[3]);
                    if (ah6 > 6'd9)
                        ah6 = ah6 + 6'd6;
                    n_p[FC] = ah6 > 6'd15;
                    n_a = {ah6[3:0], al6[3:0]};
                    n_p = nz(n_p, n_a);
                end else begin
                    bin16 = {8'h00, a} - {8'h00, opnd} - {15'h0, ~p[FC]};
                    lo_borrow = {1'b0, a[3:0]} < ({1'b0, opnd[3:0]} + {4'h0, ~p[FC]});
                    n_a = bin16[7:0];
                    if (lo_borrow)
                        n_a = n_a - 8'h06;
                    if (bin16[15])
                        n_a = n_a - 8'h60;
                    n_p[FV] = (a[7] ^ opnd[7]) & (a[7] ^ bin16[7]);
                    n_p[FC] = ~bin16[15];
                    n_p = nz(n_p, n_a);
                end
            end
            OP_SHIFT_A: begin
                sum9 = shift(cw.sel, a, p[FC]);
                n_a = sum9[7:0];
                n_p = nz(n_p, sum9[7:0]);
                n_p[FC] = sum9[8];
            end
            OP_SHIFT_AD: begin
                sum9 = shift(cw.sel, ad[7:0], p[FC]);
                n_ad = {8'h00, sum9[7:0]};
                n_p = nz(n_p, sum9[7:0]);
                n_p[FC] = sum9[8];
            end
            OP_AD_STEP: begin
                n_ad = (cw.sel == SEL_INC) ? ad + 16'd1 : ad - 16'd1;
                n_p = nz(n_p, n_ad[7:0]);
            end
            OP_A_STEP: begin
                n_a = (cw.sel == SEL_INC) ? a + 8'd1 : a - 8'd1;
                n_p = nz(n_p, n_a);
            end
            OP_X_INC: begin n_x = x + 8'd1; n_p = nz(n_p, n_x); end
            OP_X_DEC: begin n_x = x - 8'd1; n_p = nz(n_p, n_x); end
            OP_Y_INC: begin n_y = y + 8'd1; n_p = nz(n_p, n_y); end
            OP_Y_DEC: begin n_y = y - 8'd1; n_p = nz(n_p, n_y); end

            // ---- bit instructions ----
            OP_BIT_SELECT: begin
                bsel = 4'(32'(cw.sel) - 32'(SEL_B0));
                n_ad = {15'h0, ad[bsel]};
            end
            OP_RMB: begin
                bsel = 4'(32'(cw.sel) - 32'(SEL_B0));
                n_ad = ad;
                n_ad[bsel] = 1'b0;
            end
            OP_SMB: begin
                bsel = 4'(32'(cw.sel) - 32'(SEL_B0));
                n_ad = ad;
                n_ad[bsel] = 1'b1;
            end
            OP_TSB: begin
                n_p[FZ] = (a & ad[7:0]) == 8'h00;
                n_ad = {ad[15:8], ad[7:0] | a};
            end
            OP_TRB: begin
                n_p[FZ] = (a & ad[7:0]) == 8'h00;
                n_ad = {ad[15:8], ad[7:0] & ~a};
            end

            // ---- transfers ----
            OP_TAX: begin n_x = a; n_p = nz(n_p, a); end
            OP_TAY: begin n_y = a; n_p = nz(n_p, a); end
            OP_TXA: begin n_a = x; n_p = nz(n_p, x); end
            OP_TYA: begin n_a = y; n_p = nz(n_p, y); end
            OP_TSX: begin n_x = s; n_p = nz(n_p, s); end
            OP_TXS: n_s = x;

            // ---- flags ----
            OP_FLAG_CLEAR: begin
                case (cw.sel)
                    SEL_I: n_p[FI] = 1'b0;
                    SEL_D: n_p[FD] = 1'b0;
                    default: n_p[FC] = 1'b0;
                endcase
            end
            OP_FLAG_SET: begin
                case (cw.sel)
                    SEL_I: n_p[FI] = 1'b1;
                    SEL_D: n_p[FD] = 1'b1;
                    default: n_p[FC] = 1'b1;
                endcase
            end
            OP_CLV: n_p[FV] = 1'b0;
            OP_PULL_P: n_p = (gd | (8'h01 << FX)) & ~(8'h01 << FB);

            // ---- branches ----
            OP_BRANCH_TEST: begin
                n_ad = pc + {{8{gd[7]}}, gd};
                case (cw.sel)
                    SEL_N_CLR, SEL_N_SET: flagbit = p[FN];
                    SEL_V_CLR, SEL_V_SET: flagbit = p[FV];
                    SEL_C_CLR, SEL_C_SET: flagbit = p[FC];
                    default: flagbit = p[FZ];
                endcase
                case (cw.sel)
                    SEL_N_CLR, SEL_V_CLR, SEL_C_CLR, SEL_Z_CLR:
                        branch_taken = ~flagbit;
                    default: branch_taken = flagbit;
                endcase
                cond_fail = branch_taken;  // taken: keep going, no fetch yet
            end
            OP_BRANCH_ALWAYS: n_ad = pc + {{8{gd[7]}}, gd};
            OP_BRANCH_FIXUP, OP_BRANCH_FIXUP_PIP: begin
                if (ad[15:8] == pc[15:8]) begin
                    n_pc = ad;
                    pip_hold = cw.op == OP_BRANCH_FIXUP_PIP;
                end else begin
                    cond_fail = 1'b1;  // crossed: one more cycle to fix up
                end
            end
            OP_BBR_TEST, OP_BBS_TEST: begin
                branch_taken = (cw.op == OP_BBS_TEST) ? ad[0] : ~ad[0];
                if (branch_taken)
                    n_ad = pc + {{8{gd[7]}}, gd};
                cond_fail = branch_taken;  // taken: more cycles instead of fetch
            end

            // ---- interrupt entry and stalls ----
            OP_BRK_PUSH_PCH: begin
                if (!(seq_irq || seq_nmi))
                    n_pc = pc + 16'd1;
                wr_gate = !seq_res;
            end
            OP_BRK_PUSH: wr_gate = !seq_res;
            OP_BRK_VECTOR: begin
                wr_gate = !seq_res;
                n_ad = seq_res ? 16'hFFFC : (seq_nmi ? 16'hFFFA : 16'hFFFE);
            end
            OP_BRK_ENTER: begin
                n_p[FI] = 1'b1;
                n_p[FD] = 1'b0;
                n_brk_irq = 1'b0;
                n_brk_nmi = 1'b0;
                n_brk_res = 1'b0;
            end
            OP_WAI: n_wait = 1'b1;
            default: n_stop = 1'b1;  // OP_STP
        endcase
    end

    // The cycle ends here on a conditional operation whose condition failed;
    // fetch and skip belong to the condition holding.
    logic do_fetch, do_skip;
    always_comb begin
        do_fetch = cw.fetch && !cond_fail;
        do_skip = cw.skip && !cond_fail;
    end

    // ------------------------------------------------------------------
    // Address unit: post-operation values in, one bus address out
    // ------------------------------------------------------------------

    logic [15:0] base16, off16, full_addr;
    w65c02_lo_t use_lo;
    w65c02_off_t use_off;
    w65c02_hi_t use_hi;
    w65c02_post_t use_post;

    always_comb begin
        if (cond_fail) begin
            use_lo = cw.xlo;
            use_off = cw.xoff;
            use_hi = cw.xhi;
            use_post = cw.xpost;
        end else begin
            use_lo = cw.lo;
            use_off = cw.off;
            use_hi = cw.hi;
            use_post = cw.post;
        end

        case (use_lo)
            LO_PC: base16 = n_pc;
            // A few cycles read through AD while replacing it; ad_pre marks
            // them and the address takes the pre-operation value.
            LO_AD: base16 = cw.ad_pre ? ad : n_ad;
            LO_S: base16 = {8'h00, n_s};
            LO_GD: base16 = {8'h00, gd};
            LO_CONST_7F: base16 = 16'h007F;
            default: base16 = 16'h0000;  // LO_ZERO, LO_HOLD
        endcase

        case (use_off)
            OFF_1: off16 = 16'h0001;
            OFF_X: off16 = {8'h00, x};
            OFF_Y: off16 = {8'h00, y};
            OFF_M1: off16 = 16'hFFFF;
            OFF_M2: off16 = 16'hFFFE;
            default: off16 = 16'h0000;  // OFF_0
        endcase

        case (use_hi)
            HI_CARRY: full_addr = base16 + off16;
            HI_ZP: full_addr = {8'h00, base16[7:0] + off16[7:0]};
            HI_STACK: full_addr = {8'h01, base16[7:0]};
            HI_PC: full_addr = {n_pc[15:8], base16[7:0]};
            HI_AD: full_addr = {(cw.ad_pre ? ad[15:8] : n_ad[15:8]),
                                base16[7:0] + off16[7:0]};
            HI_GD: full_addr = {gd, base16[7:0]};
            HI_ZERO: full_addr = {8'h00, base16[7:0]};
            default: full_addr = w65c02_addr;  // HI_HOLD
        endcase

        if (do_fetch)
            full_addr = n_pc;
    end

    // ------------------------------------------------------------------
    // Data out
    // ------------------------------------------------------------------

    logic [7:0] dout;
    logic n_we;
    always_comb begin
        case (cw.dout)
            DOUT_A: dout = n_a;
            DOUT_X: dout = n_x;
            DOUT_Y: dout = n_y;
            DOUT_AD: dout = n_ad[7:0];
            DOUT_PCL: dout = n_pc[7:0];
            DOUT_PCH: dout = n_pc[15:8];
            DOUT_P_BRK: dout = p | (8'h01 << FB) | (8'h01 << FX);
            DOUT_P_ENTRY: dout = p | (8'h01 << FX)
                | ((seq_irq || seq_nmi) ? 8'h00 : (8'h01 << FB));
            default: dout = 8'h00;  // DOUT_ZERO, DOUT_NONE
        endcase
        n_we = cw.wr && wr_gate;
    end

    // ------------------------------------------------------------------
    // Commit
    // ------------------------------------------------------------------

    logic [15:0] post_pc, post_ad;
    logic [7:0] post_s;
    always_comb begin
        post_pc = n_pc;
        post_ad = n_ad;
        post_s = n_s;
        case (use_post)
            POST_PC_INC: post_pc = n_pc + 16'd1;
            POST_AD_INC: post_ad = n_ad + 16'd1;
            POST_S_INC: post_s = n_s + 8'd1;
            POST_S_DEC: post_s = n_s - 8'd1;
            default: ;  // POST_NONE
        endcase
    end

    logic [15:0] pip_mask;
    always_comb pip_mask = w65c02_sync ? 16'h03FF : 16'hFFFF;

    /* The bus one edge early. When the core is stalled the registers
     * hold, so the answer is what they already carry. */
    always_comb begin
        if (stop_stall || wait_stall || rdy_stall) begin
            w65c02_next_addr = w65c02_addr;
            w65c02_next_data = w65c02_data;
            w65c02_next_we = w65c02_we;
        end else begin
            w65c02_next_addr = full_addr;
            w65c02_next_data = dout;
            w65c02_next_we = n_we;
        end
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            // The C model comes out of init with SYNC and RES pending at
            // address zero, so the first cycle forces the reset sequence.
            pc <= 16'h0000;
            ad <= 16'h0000;
            a <= 8'h00;
            x <= 8'h00;
            y <= 8'h00;
            s <= 8'h00;
            p <= 8'h02;
            ir <= 8'h00;
            tick <= 3'd0;
            irq_pip <= 16'h0000;
            nmi_pip <= 16'h0000;
            brk_irq <= 1'b0;
            brk_nmi <= 1'b0;
            brk_res <= 1'b0;
            wait_flag <= 1'b0;
            stop_flag <= 1'b0;
            nmi_prev <= 1'b0;
            res_seen <= 1'b1;
            w65c02_addr <= 16'h0000;
            w65c02_data <= 8'h00;
            w65c02_we <= 1'b0;
            w65c02_sync <= 1'b1;
        end else if (st_jam) begin
            /* All five words on one edge, ahead of en. A restore
             * happens with the clock back on, and a jam spread over
             * five edges would let the core run through the four it was
             * not being written on. One edge is the whole state and no
             * instruction in between. */
            {s, y, x, a} <= st_jam_data[0];
            {ir, p, pc} <= st_jam_data[1];
            ad <= st_jam_data[2][31:16];
            tick <= st_jam_data[2][10:8];
            res_seen <= st_jam_data[2][7];
            nmi_prev <= st_jam_data[2][6];
            stop_flag <= st_jam_data[2][5];
            wait_flag <= st_jam_data[2][4];
            brk_res <= st_jam_data[2][3];
            brk_nmi <= st_jam_data[2][2];
            brk_irq <= st_jam_data[2][1];
            w65c02_we <= st_jam_data[2][0];
            {irq_pip, nmi_pip} <= st_jam_data[3];
            w65c02_addr <= st_jam_data[4][31:16];
            w65c02_data <= st_jam_data[4][15:8];
            w65c02_sync <= st_jam_data[4][0];
        end else if (en) begin
            nmi_prev <= nmi_i;
            if (stop_stall || wait_stall) begin
                // Only the pin moves; the C returns before its prologue.
            end else if (rdy_stall) begin
                irq_pip <= irq_pip_det << 1;
                nmi_pip <= nmi_pip_det;
            end else begin
                pc <= post_pc;
                ad <= post_ad;
                a <= n_a;
                x <= n_x;
                y <= n_y;
                s <= post_s;
                p <= n_p;
                ir <= eff_ir;
                tick <= do_fetch ? 3'd0
                    : eff_tick + (do_skip ? 3'd2 : 3'd1);
                if (pip_hold) begin
                    irq_pip <= ((irq_pip_det & pip_mask) >> 1) << 1;
                    nmi_pip <= ((nmi_pip_det & pip_mask) >> 1) << 1;
                end else begin
                    irq_pip <= (irq_pip_det & pip_mask) << 1;
                    nmi_pip <= (nmi_pip_det & pip_mask) << 1;
                end
                wait_flag <= n_wait;
                stop_flag <= n_stop;
                brk_irq <= n_brk_irq;
                brk_nmi <= n_brk_nmi;
                brk_res <= n_brk_res;
                res_seen <= take_res ? 1'b0 : res_seen;
                w65c02_addr <= full_addr;
                w65c02_data <= dout;
                w65c02_we <= n_we;
                w65c02_sync <= do_fetch;
            end
        end
    end

endmodule
