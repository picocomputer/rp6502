/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

module sst_engine
    import rp6502_pkg::*;
(
    input logic clk_sys,
    input logic rst_n,

    input logic sst_save,

    input logic sst_load,
    output logic sst_engine_load_done,
    output logic sst_engine_load_err,

    output logic sst_engine_busy,
    output logic sst_engine_freeze,
    input logic running,

    output logic sst_engine_arr_own,

    output logic sst_engine_hold_res,
    output logic sst_engine_dbg_halt,
    input logic dbg_halted,

    output logic sst_engine_ready,
    input logic [17:0] rd_idx,
    input logic rd_t,
    output logic [31:0] sst_engine_rdata,
    output logic sst_engine_rvalid,

    output logic sst_engine_stage_pend,
    output logic [27:0] sst_engine_stage_addr,
    input logic stage_stall,
    input logic [7:0] stage_rdata,

    output logic [13:0] sst_engine_mem_addr,
    output logic [31:0] sst_engine_mem_wdata,
    output logic sst_engine_xram_we,
    output logic sst_engine_sram_sel,
    output logic [15:0] sst_engine_sram_addr,
    output logic sst_engine_sram_we,
    output logic [7:0] sst_engine_sram_wdata,
    input logic [7:0] sram_rdata,
    input logic sram_stall,

    output logic [7:0] sst_engine_regs_word,
    output logic sst_engine_regs_we,
    output logic [31:0] sst_engine_regs_wdata,
    input logic [31:0] regs_rdata,

    output logic sst_engine_cell_we,
    output logic sst_engine_xprog_we,
    output logic [1:0] sst_engine_xprog_word,
    input logic [31:0] xram_rdata,
    input logic [31:0] cell_rdata,
    input logic [31:0] xprog_rdata,

    output logic sst_engine_tcm_sel,
    output logic [14:0] sst_engine_tcm_addr,
    output logic sst_engine_tcm_we,
    output logic [31:0] sst_engine_tcm_wdata,
    input logic [31:0] tcm_rdata,

    output logic [1:0] sst_engine_st_sel,
    output logic [2:0] sst_engine_st_idx,
    input logic [31:0] st_rdata,
    output logic sst_engine_st_jam,
    output logic sst_engine_mtime_jam,
    output logic [31:0] sst_engine_jam_mach[4],
    output logic [31:0] sst_engine_jam_cpu[5],
    output logic [31:0] sst_engine_jam_via[7],
    output logic [31:0] sst_engine_jam_ria[12],

    output logic [31:0] sst_engine_dbg_data0,
    output logic sst_engine_dbg_resume,
    output logic [31:0] sst_engine_dbg_instr,
    output logic sst_engine_dbg_instr_vld,
    input logic dbg_instr_rdy,
    input logic dbg_ebreak,
    input logic [31:0] dbg_data0,
    input logic dbg_data0_wen
);

    localparam int W_HDR = 16;
    localparam int W_STATE = 64;
    localparam int W_REGS = 256;
    localparam int W_SRAM = 16384;
    localparam int W_XRAM = 16384;
    localparam int W_CELLS = 15360;
    localparam int W_XPROG = 8192;
    localparam int W_TCM = 24576;
    localparam int W_END = 4;

    localparam int B_HDR = 0;
    localparam int B_STATE = B_HDR + W_HDR;
    localparam int B_REGS = B_STATE + W_STATE;
    localparam int B_SRAM = B_REGS + W_REGS;
    localparam int B_XRAM = B_SRAM + W_SRAM;
    localparam int B_CELLS = B_XRAM + W_XRAM;
    localparam int B_XPROG = B_CELLS + W_CELLS;
    localparam int B_TCM = B_XPROG + W_XPROG;
    localparam int B_END = B_TCM + W_TCM;
    localparam int W_TOTAL = B_END + W_END;

    localparam logic [31:0] SST_MAGIC = 32'h52365353;
    localparam logic [31:0] SST_END_MAGIC = 32'h52365345;
    localparam logic [31:0] SST_VERSION = 32'd2;

    localparam int A_STAGE_OFF = 32'h03F0_0000;

    localparam int REGS_HOLE = 16;

    localparam int ST_CPU = 4;
    localparam int ST_VIA = 9;
    localparam int ST_RV = 16;
    localparam logic [1:0] SEL_MACH = 2'd0;
    localparam logic [1:0] SEL_CPU = 2'd1;
    localparam logic [1:0] SEL_VIA = 2'd2;

    localparam int RV_WORDS = 32;
    logic [31:0] rvreg[RV_WORDS];
    logic [31:0] flopreg[ST_RV];
    localparam int RIA_JAM = 12;
    logic [31:0] riareg[RIA_JAM];
    logic ria_flop;
    logic [3:0] ria_slot;
    always_comb begin
        ria_flop = 1'b1;
        if (off < 18'd8) ria_slot = 4'(off);
        else if (off == 18'd17) ria_slot = 4'd8;
        else if (off == 18'd18) ria_slot = 4'd9;
        else if (off == 18'd200) ria_slot = 4'd11;
        else begin
            ria_slot = 4'd0;
            ria_flop = 1'b0;
        end
    end
    logic [4:0] rv_n;

    localparam logic [11:0] CSR_DMDATA0 = 12'hBFF;
    localparam logic [11:0] CSR_DPC = 12'h7B1;
    localparam logic [31:0] I_EBREAK = 32'h00100073;

    function automatic logic [31:0] i_reg_out(input logic [4:0] n);
        return {CSR_DMDATA0, n, 3'b001, 5'd0, 7'b1110011};
    endfunction
    function automatic logic [31:0] i_reg_in(input logic [4:0] n);
        return {CSR_DMDATA0, 5'd0, 3'b010, n, 7'b1110011};
    endfunction
    function automatic logic [31:0] i_csr_write(input logic [11:0] csr,
                                                input logic [4:0] n);
        return {csr, n, 3'b001, 5'd0, 7'b1110011};
    endfunction
    function automatic logic [31:0] i_csr_read(input logic [11:0] csr,
                                               input logic [4:0] n);
        return {csr, 5'd0, 3'b010, n, 7'b1110011};
    endfunction

    typedef enum logic [5:0] {
        S_IDLE,
        S_FREEZE,
        S_HALT,
        S_SPILL_ARM,
        S_SPILL_ISSUE,
        S_SPILL_TAKE,
        S_SPILL_BRK_ARM,
        S_SPILL_BRK,
        S_SPILL_WAIT,
        S_READY,
        S_READ_ARM,
        S_READ,
        S_READ_WAIT,
        S_LD_FREEZE,
        S_LD_HALT,
        S_LD_FETCH,
        S_LD_SCAN,
        S_LD_CHK,
        S_LD_BAD,
        S_LD_PUT,
        S_LD_PUT_WAIT,
        S_LD_PUT_DONE,
        S_RESUME,
        S_LD_JAM,
        S_LD_JAM2,
        S_LD_DONE,
        S_LD_ACK,
        S_INJ_ARM,
        S_INJ_ISSUE,
        S_INJ_TAKE,
        S_INJ_BRK_ARM,
        S_INJ_BRK,
        S_INJ_WAIT
    } state_t;
    state_t state ;

    logic [31:0] data0_q;

    logic spill_dpc;
    logic [1:0] spill_step;
    logic [31:0] spill_instr;

    logic [17:0] hold_idx ;
    logic [31:0] hold ;
    logic hold_valid ;
    logic [1:0] byte_n;
    logic [23:0] acc;
    logic [17:0] off, dec_idx;

    logic ready_q, rvalid_q, done_q;
    always_comb begin
        sst_engine_busy = state != S_IDLE;
        sst_engine_freeze = want_stop;
        sst_engine_arr_own = arr_own;
        sst_engine_hold_res = hold_res;
        sst_engine_dbg_halt = state != S_IDLE && state != S_RESUME
            && state != S_LD_DONE && state != S_LD_ACK
            && state != S_LD_JAM && state != S_LD_JAM2;
        sst_engine_load_done = done_q;
        sst_engine_load_err = ld_bad;
        sst_engine_ready = ready_q;
        sst_engine_rdata = hold;
        sst_engine_rvalid = rvalid_q;
        sst_engine_dbg_instr = spill_instr;
        sst_engine_dbg_instr_vld = state == S_SPILL_ARM
            || state == S_SPILL_ISSUE || state == S_SPILL_BRK_ARM
            || state == S_SPILL_BRK || state == S_INJ_ARM
            || state == S_INJ_ISSUE || state == S_INJ_BRK_ARM
            || state == S_INJ_BRK;
        sst_engine_dbg_data0 = inj_val;
        sst_engine_dbg_resume = state == S_LD_DONE || state == S_RESUME;
    end

    always_comb begin
        off = '0;
        dec_idx = ld_writing ? ld_idx : hold_idx;
        if (dec_idx >= 18'(B_TCM)) off = dec_idx - 18'(B_TCM);
        else if (dec_idx >= 18'(B_XPROG)) off = dec_idx - 18'(B_XPROG);
        else if (dec_idx >= 18'(B_CELLS)) off = dec_idx - 18'(B_CELLS);
        else if (dec_idx >= 18'(B_XRAM)) off = dec_idx - 18'(B_XRAM);
        else if (dec_idx >= 18'(B_SRAM)) off = dec_idx - 18'(B_SRAM);
        else if (dec_idx >= 18'(B_REGS)) off = dec_idx - 18'(B_REGS);
    end

    logic on_tcm;
    logic on_tcm_q;
    logic [17:0] off_q;
    logic [1:0] st_sel_q;
    logic [2:0] st_idx_q;

    logic on_sram, on_xram, on_cell, on_xprog, on_regs;
    logic on_sram_q, on_xram_q, on_cell_q, on_xprog_q, on_regs_q;
    logic ria_flop_q;
    logic on_arr_q;
    always_comb begin
        on_sram = dec_idx >= 18'(B_SRAM) && dec_idx < 18'(B_XRAM);
        on_xram = dec_idx >= 18'(B_XRAM) && dec_idx < 18'(B_CELLS);
        on_cell = dec_idx >= 18'(B_CELLS) && dec_idx < 18'(B_XPROG);
        on_xprog = dec_idx >= 18'(B_XPROG) && dec_idx < 18'(B_TCM);
        on_arr_q = on_xram_q || on_cell_q || on_xprog_q;

        sst_engine_sram_sel = on_sram_q && !sram_gap
            && (state == S_READ || state == S_LD_PUT);
        sst_engine_regs_word = off_q[7:0];
        sst_engine_regs_we = on_regs_q && state == S_LD_PUT && !ria_flop_q;
        sst_engine_regs_wdata = ld_word;
        sst_engine_sram_addr = 16'({off_q, 2'd0} + {18'd0, byte_n});
        sst_engine_sram_we = on_sram_q && state == S_LD_PUT;
        sst_engine_sram_wdata = ld_word[31 - {byte_n, 3'd0} -: 8];
        sst_engine_mem_addr = on_xprog_q ? 14'(off_q >> 2) : off_q[13:0];
        sst_engine_mem_wdata = ld_word;
        sst_engine_xram_we = on_xram_q && state == S_LD_PUT;
        sst_engine_cell_we = on_cell_q && state == S_LD_PUT;
        sst_engine_xprog_we = on_xprog_q && state == S_LD_PUT;
        sst_engine_xprog_word = off_q[1:0];

        on_regs = dec_idx >= 18'(B_REGS) && dec_idx < 18'(B_SRAM)
            && dec_idx != 18'(B_REGS + REGS_HOLE);
        on_tcm = dec_idx >= 18'(B_TCM) && dec_idx < 18'(B_END);
        sst_engine_tcm_sel = on_tcm_q && (state == S_READ
            || state == S_READ_WAIT || state == S_LD_PUT
            || state == S_LD_PUT_WAIT);
        sst_engine_tcm_addr = off_q[14:0];
        sst_engine_tcm_we = on_tcm_q && state == S_LD_PUT;
        sst_engine_tcm_wdata = ld_word;
        sst_engine_stage_pend = state == S_LD_FETCH;
        sst_engine_stage_addr = 28'(A_STAGE_OFF)
            + {8'd0, ld_idx, 2'd0} + {16'd0, ld_off, 2'd0}
            + {26'd0, byte_n};
    end

    logic [31:0] state_word;
    logic on_flops;
    logic [17:0] st_off;
    logic [1:0] st_sel;
    logic [2:0] st_idx;
    always_comb begin
        st_off = dec_idx - 18'(B_STATE);
        on_flops = dec_idx >= 18'(B_STATE) && dec_idx < 18'(B_STATE + ST_RV);
        if (st_off >= 18'(ST_VIA)) begin
            st_sel = SEL_VIA;
            st_idx = 3'(st_off - 18'(ST_VIA));
        end else if (st_off >= 18'(ST_CPU)) begin
            st_sel = SEL_CPU;
            st_idx = 3'(st_off - 18'(ST_CPU));
        end else begin
            st_sel = SEL_MACH;
            st_idx = 3'(st_off);
        end
        sst_engine_st_sel = st_sel_q;
        sst_engine_st_idx = st_idx_q;
        sst_engine_st_jam = state == S_LD_JAM || state == S_LD_JAM2;
        sst_engine_mtime_jam = !inj_save
            && (state == S_LD_PUT_DONE
                || state == S_INJ_ARM || state == S_INJ_ISSUE
                || state == S_INJ_TAKE || state == S_INJ_BRK_ARM
                || state == S_INJ_BRK || state == S_INJ_WAIT);
        for (int i = 0; i < 4; i++)
            sst_engine_jam_mach[i] = flopreg[i];
        for (int i = 0; i < 5; i++)
            sst_engine_jam_cpu[i] = flopreg[ST_CPU + i];
        for (int i = 0; i < 7; i++)
            sst_engine_jam_via[i] = flopreg[ST_VIA + i];
        for (int i = 0; i < RIA_JAM; i++)
            sst_engine_jam_ria[i] = riareg[i];
        state_word = st_rdata;
        if (hold_idx >= 18'(B_STATE + ST_RV))
            state_word = rvreg[5'(hold_idx - 18'(B_STATE + ST_RV))];
    end

    logic [31:0] hdr_word;
    always_comb begin
        case (hold_idx[3:0])
            4'd0: hdr_word = SST_MAGIC;
            4'd1: hdr_word = SST_VERSION;
            4'd2: hdr_word = 32'(W_TOTAL) << 2;
            default: hdr_word = 32'd0;
        endcase
    end

    logic [31:0] end_word;
    logic [31:0] sum ;
    always_comb begin
        case (hold_idx - 18'(B_END))
            18'd0: end_word = sum;
            18'd1: end_word = 32'(W_TOTAL);
            18'd2: end_word = SST_END_MAGIC;
            default: end_word = 32'd0;
        endcase
    end

    logic direct;
    logic [31:0] direct_word;
    always_comb begin
        direct = !on_sram && !on_xram && !on_cell && !on_xprog
            && !on_regs && !on_tcm;
        direct_word = hold_idx < 18'(B_STATE) ? hdr_word
            : (hold_idx < 18'(B_REGS) ? state_word
               : (hold_idx >= 18'(B_END) ? end_word : 32'd0));
    end

    logic summing ;
    logic [17:0] sum_idx ;

    logic [31:0] inj_val;
    logic inj_dpc;
    logic inj_save;
    logic [1:0] inj_step;
    logic [17:0] ld_idx;
    logic [31:0] ld_word;
    logic ld_writing;

    logic want_stop, hold_res, arr_own, sram_gap;
    (* preserve *) logic running_s1, running_s2;
    logic ld_verify, ld_bad;
    localparam logic [9:0] SCAN_CAP = 10'd1023;
    logic [9:0] ld_off;
    logic ld_scan;
    logic [31:0] vsum ;
    logic [17:0] bad_idx ;
    logic [31:0] bad_word ;

    (* preserve *) logic save_s1, save_s2;
    logic save_req;
    always_comb save_req = save_s2;
    (* preserve *) logic load_s1, load_s2;
    logic load_req;
    always_comb load_req = load_s2;

    (* preserve *) logic idx_t1, idx_t2, idx_t3;
    logic [17:0] req_idx;
    logic req_pending;
    logic req_new;
    always_comb req_new = idx_t2 != idx_t3;

    always_ff @(posedge clk_sys) begin
        if (!rst_n) begin
            state <= S_IDLE;
            running_s1 <= 1'b1;
            running_s2 <= 1'b1;
            hold_res <= 1'b0;
            arr_own <= 1'b0;
            sram_gap <= 1'b0;
            hold_valid <= 1'b0;
            hold_idx <= '0;
            hold <= '0;
            byte_n <= '0;
            acc <= '0;
            rv_n <= 5'd1;
            spill_dpc <= 1'b0;
            spill_step <= '0;
            spill_instr <= I_EBREAK;
            data0_q <= '0;
            sum <= '0;
            summing <= 1'b0;
            sum_idx <= '0;
            ready_q <= 1'b0;
            rvalid_q <= 1'b0;
            done_q <= 1'b0;
            on_sram_q <= 1'b0;
            on_xram_q <= 1'b0;
            on_cell_q <= 1'b0;
            on_xprog_q <= 1'b0;
            on_regs_q <= 1'b0;
            ria_flop_q <= 1'b0;
            on_tcm_q <= 1'b0;
            off_q <= '0;
            st_sel_q <= SEL_MACH;
            st_idx_q <= '0;
            save_s1 <= 1'b0;
            save_s2 <= 1'b0;
            load_s1 <= 1'b0;
            load_s2 <= 1'b0;
            ld_idx <= '0;
            ld_word <= '0;
            ld_writing <= 1'b0;
            want_stop <= 1'b0;
            ld_verify <= 1'b0;
            ld_off <= '0;
            ld_scan <= 1'b0;
            ld_bad <= 1'b0;
            vsum <= '0;
            bad_idx <= '0;
            bad_word <= '0;
            inj_val <= '0;
            inj_dpc <= 1'b0;
            inj_save <= 1'b0;
            inj_step <= '0;
            idx_t1 <= 1'b0;
            idx_t2 <= 1'b0;
            idx_t3 <= 1'b0;
            req_idx <= '0;
            req_pending <= 1'b0;
        end else begin
            ready_q <= !summing && (state == S_READY || state == S_READ_ARM
                || state == S_READ || state == S_READ_WAIT);
            on_sram_q <= on_sram;
            on_xram_q <= on_xram;
            on_cell_q <= on_cell;
            on_xprog_q <= on_xprog;
            on_regs_q <= on_regs;
            ria_flop_q <= ria_flop;
            on_tcm_q <= on_tcm;
            off_q <= off;
            st_sel_q <= st_sel;
            st_idx_q <= st_idx;
            done_q <= state == S_LD_DONE || state == S_LD_ACK;
            rvalid_q <= hold_valid && hold_idx == req_idx;
            save_s1 <= sst_save;
            save_s2 <= save_s1;
            load_s1 <= sst_load;
            load_s2 <= load_s1;
            idx_t1 <= rd_t;
            idx_t2 <= idx_t1;
            idx_t3 <= idx_t2;
            if (req_new) begin
                req_idx <= rd_idx;
                req_pending <= 1'b1;
            end
            running_s1 <= running;
            running_s2 <= running_s1;
            arr_own <= want_stop && !running_s2;
            if (dbg_data0_wen) data0_q <= dbg_data0;
            case (state)
                S_IDLE: begin
                    hold_valid <= 1'b0;
                    want_stop <= 1'b0;
                    hold_res <= 1'b0;
                    sum <= '0;
                    sum_idx <= '0;
                    summing <= 1'b0;
                    rv_n <= 5'd1;
                    spill_dpc <= 1'b0;
                    spill_step <= '0;
                    if (save_req) state <= S_FREEZE;
                    else if (load_req) state <= S_LD_FREEZE;
                end

                S_FREEZE: state <= S_HALT;
                S_HALT: begin
                    if (dbg_halted) begin
                        spill_instr <= i_reg_out(rv_n);
                        state <= S_SPILL_ARM;
                    end
                end

                S_SPILL_ARM: if (dbg_instr_rdy) state <= S_SPILL_ISSUE;
                S_SPILL_ISSUE: if (!dbg_instr_rdy) state <= S_SPILL_TAKE;
                S_SPILL_TAKE: begin
                    spill_instr <= I_EBREAK;
                    state <= S_SPILL_BRK_ARM;
                end
                S_SPILL_BRK_ARM: if (dbg_instr_rdy) state <= S_SPILL_BRK;
                S_SPILL_BRK: if (!dbg_instr_rdy) state <= S_SPILL_WAIT;
                S_SPILL_WAIT:
                if (dbg_ebreak) begin
                    if (spill_step == 2'd0 && spill_dpc) begin
                        spill_step <= 2'd1;
                        spill_instr <= i_reg_out(5'd31);
                        state <= S_SPILL_ARM;
                    end else if (spill_dpc) begin
                        rvreg[0] <= data0_q;
                        want_stop <= 1'b1;
                        summing <= 1'b1;
                        sum_idx <= '0;
                        sum <= '0;
                        hold_valid <= 1'b0;
                        inj_save <= 1'b1;
                        inj_dpc <= 1'b0;
                        rv_n <= 5'd31;
                        inj_val <= rvreg[31];
                        spill_instr <= i_reg_in(5'd31);
                        state <= S_INJ_ARM;
                    end else begin
                        rvreg[rv_n] <= data0_q;
                        if (rv_n == 5'd31) begin
                            spill_dpc <= 1'b1;
                            spill_step <= 2'd0;
                            spill_instr <= i_csr_read(CSR_DPC, 5'd31);
                            state <= S_SPILL_ARM;
                        end else begin
                            rv_n <= rv_n + 5'd1;
                            spill_instr <= i_reg_out(rv_n + 5'd1);
                            state <= S_SPILL_ARM;
                        end
                    end
                end

                S_LD_FREEZE: state <= S_LD_HALT;
                S_LD_HALT:
                if (dbg_halted && !running_s2) begin
                    ld_idx <= '0;
                    ld_off <= '0;
                    ld_scan <= 1'b1;
                    byte_n <= '0;
                    acc <= '0;
                    vsum <= '0;
                    ld_bad <= 1'b0;
                    ld_verify <= 1'b0;
                    state <= S_LD_FETCH;
                end else if (dbg_halted) want_stop <= 1'b1;

                S_LD_FETCH:
                if (!stage_stall) begin
                    acc <= {acc[15:0], stage_rdata};
                    if (byte_n == 2'd3) begin
                        ld_word <= {acc, stage_rdata};
                        byte_n <= '0;
                        state <= ld_scan ? S_LD_SCAN
                            : (ld_verify ? S_LD_CHK : S_LD_PUT);
                    end else byte_n <= byte_n + 2'd1;
                end

                S_LD_SCAN:
                if ((ld_idx == 18'd0 && ld_word != SST_MAGIC)
                    || (ld_idx == 18'd1 && ld_word != SST_VERSION)
                    || (ld_idx == 18'd2
                        && ld_word != (32'(W_TOTAL) << 2))) begin
                    if (ld_off == SCAN_CAP) state <= S_LD_BAD;
                    else begin
                        ld_off <= ld_off + 10'd1;
                        ld_idx <= '0;
                        state <= S_LD_FETCH;
                    end
                end else if (ld_idx == 18'd2) begin
                    ld_scan <= 1'b0;
                    ld_verify <= 1'b1;
                    ld_idx <= '0;
                    vsum <= '0;
                    state <= S_LD_FETCH;
                end else begin
                    ld_idx <= ld_idx + 18'd1;
                    state <= S_LD_FETCH;
                end

                S_LD_CHK: begin
                    if (ld_idx < 18'(B_END))
                        vsum <= {vsum[30:0], vsum[31]} + ld_word;
                    if ((ld_idx == 18'(B_HDR) && ld_word != SST_MAGIC)
                        || (ld_idx == 18'(B_HDR + 1) && ld_word != SST_VERSION)
                        || (ld_idx == 18'(B_HDR + 2)
                            && ld_word != (32'(W_TOTAL) << 2))
                        || (ld_idx == 18'(B_END) && ld_word != vsum)
                        || (ld_idx == 18'(B_END + 1)
                            && ld_word != 32'(W_TOTAL))
                        || (ld_idx == 18'(B_END + 2)
                            && ld_word != SST_END_MAGIC))
                        state <= S_LD_BAD;
                    else if (ld_idx == 18'(B_END + 2)) begin
                        ld_idx <= 18'(B_STATE);
                        ld_verify <= 1'b0;
                        ld_writing <= 1'b1;
                        state <= S_LD_FETCH;
                    end else begin
                        ld_idx <= ld_idx + 18'd1;
                        state <= S_LD_FETCH;
                    end
                end

                S_LD_BAD: begin
                    ld_bad <= 1'b1;
                    want_stop <= 1'b0;
                    bad_idx <= ld_idx;
                    bad_word <= ld_word;
                    ld_verify <= 1'b0;
                    state <= S_LD_DONE;
                end

                S_LD_PUT: begin
                    if (ld_idx >= 18'(B_STATE + ST_RV)
                        && ld_idx < 18'(B_STATE + ST_RV + RV_WORDS))
                        rvreg[5'(ld_idx - 18'(B_STATE + ST_RV))] <= ld_word;
                    if (on_flops)
                        flopreg[4'(ld_idx - 18'(B_STATE))] <= ld_word;
                    if (on_regs && ria_flop) riareg[ria_slot] <= ld_word;
                    if (on_tcm_q) begin
                        byte_n <= byte_n + 2'd1;
                        if (byte_n == 2'd3) state <= S_LD_PUT_WAIT;
                    end else if (on_arr_q || on_regs_q || on_flops)
                        state <= S_LD_PUT_WAIT;
                    else if (on_sram_q) begin
                        if (sram_gap) sram_gap <= 1'b0;
                        else if (!sram_stall) begin
                            if (byte_n == 2'd3) state <= S_LD_PUT_WAIT;
                            else byte_n <= byte_n + 2'd1;
                            sram_gap <= 1'b1;
                        end
                    end else state <= S_LD_PUT_WAIT;
                end
                S_LD_PUT_WAIT:
                begin
                    byte_n <= '0;
                    if (ld_idx == 18'(B_END - 1)) begin
                        ld_writing <= 1'b0;
                        state <= S_LD_PUT_DONE;
                    end else begin
                        ld_idx <= ld_idx + 18'd1;
                        state <= S_LD_FETCH;
                    end
                end

                S_LD_PUT_DONE: begin
                    hold_res <= 1'b1;
                    want_stop <= 1'b0;
                    if (running_s2) begin
                        ld_bad <= 1'b0;
                        inj_dpc <= 1'b1;
                        inj_step <= 2'd0;
                        inj_val <= rvreg[0];
                        rv_n <= 5'd31;
                        spill_instr <= i_reg_in(5'd31);
                        state <= S_INJ_ARM;
                    end
                end

                S_INJ_ARM: if (dbg_instr_rdy) state <= S_INJ_ISSUE;
                S_INJ_ISSUE: if (!dbg_instr_rdy) state <= S_INJ_TAKE;
                S_INJ_TAKE: begin
                    if (inj_dpc && inj_step == 2'd0) begin
                        inj_step <= 2'd1;
                        spill_instr <= i_csr_write(CSR_DPC, 5'd31);
                        state <= S_INJ_ARM;
                    end else begin
                        spill_instr <= I_EBREAK;
                        state <= S_INJ_BRK_ARM;
                    end
                end
                S_INJ_BRK_ARM: if (dbg_instr_rdy) state <= S_INJ_BRK;
                S_INJ_BRK: if (!dbg_instr_rdy) state <= S_INJ_WAIT;
                S_INJ_WAIT:
                if (dbg_ebreak) begin
                    if (inj_dpc) begin
                        inj_dpc <= 1'b0;
                        rv_n <= 5'd1;
                        inj_val <= rvreg[1];
                        spill_instr <= i_reg_in(5'd1);
                        state <= S_INJ_ARM;
                    end else if (rv_n == 5'd31) begin
                        inj_save <= 1'b0;
                        state <= inj_save ? S_READY : S_LD_DONE;
                    end else begin
                        rv_n <= rv_n + 5'd1;
                        inj_val <= rvreg[5'(rv_n + 5'd1)];
                        spill_instr <= i_reg_in(rv_n + 5'd1);
                        state <= S_INJ_ARM;
                    end
                end

                S_LD_DONE: if (!dbg_halted) state <= S_LD_ACK;

                S_RESUME: if (!dbg_halted) state <= S_IDLE;

                S_LD_ACK:
                if (!load_req) state <= ld_bad ? S_IDLE : S_LD_JAM;

                S_LD_JAM: begin
                    hold_res <= 1'b0;
                    state <= S_LD_JAM2;
                end
                S_LD_JAM2: state <= S_IDLE;

                S_READY: begin
                    if (!save_req) begin
                        want_stop <= 1'b0;
                        state <= S_RESUME;
                    end
                    else if (summing && running_s2) begin
                    end else if (summing) begin
                        if (hold_valid) begin
                            sum <= {sum[30:0], sum[31]} + hold;
                            hold_valid <= 1'b0;
                            if (sum_idx == 18'(B_END - 1)) summing <= 1'b0;
                            else begin
                                sum_idx <= sum_idx + 18'd1;
                                hold_idx <= sum_idx + 18'd1;
                                byte_n <= '0;
                                acc <= '0;
                                state <= S_READ_ARM;
                            end
                        end else begin
                            hold_idx <= sum_idx;
                            byte_n <= '0;
                            acc <= '0;
                            state <= S_READ_ARM;
                        end
                    end
                    else if (save_req && req_pending) begin
                        req_pending <= 1'b0;
                        hold_idx <= req_idx;
                        hold_valid <= 1'b0;
                        byte_n <= '0;
                        acc <= '0;
                        state <= S_READ_ARM;
                    end
                end

                S_READ_ARM: state <= S_READ;

                S_READ:
                if (direct) begin
                    hold <= direct_word;
                    hold_valid <= 1'b1;
                    state <= S_READY;
                end else if (on_arr_q) begin
                    state <= S_READ_WAIT;
                end else if (on_regs_q) begin
                    state <= S_READ_WAIT;
                end else if (on_sram_q) begin
                    if (sram_gap) sram_gap <= 1'b0;
                    else if (!sram_stall) begin
                        if (byte_n == 2'd3) begin
                            hold <= {acc, sram_rdata};
                            hold_valid <= 1'b1;
                            byte_n <= '0;
                            sram_gap <= 1'b0;
                            state <= S_READY;
                        end else begin
                            acc <= {acc[15:0], sram_rdata};
                            byte_n <= byte_n + 2'd1;
                            sram_gap <= 1'b1;
                        end
                    end
                end else if (on_tcm_q) begin
                    byte_n <= byte_n + 2'd1;
                    if (byte_n == 2'd3) state <= S_READ_WAIT;
                end

                S_READ_WAIT: begin
                    if (on_arr_q) begin
                        hold <= on_xram_q ? xram_rdata
                            : (on_cell_q ? cell_rdata : xprog_rdata);
                        hold_valid <= 1'b1;
                        byte_n <= '0;
                        state <= S_READY;
                    end else if (on_regs_q) begin
                        hold <= regs_rdata;
                        hold_valid <= 1'b1;
                        state <= S_READY;
                    end else if (on_tcm_q) begin
                        hold <= tcm_rdata;
                        hold_valid <= 1'b1;
                        byte_n <= '0;
                        state <= S_READY;
                    end else begin
                        hold_valid <= 1'b1;
                        state <= S_READY;
                    end
                end

                default: state <= S_IDLE;
            endcase

        end
    end

    logic unused_sst_engine;
    always_comb unused_sst_engine = 1'b0;

endmodule
