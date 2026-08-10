/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * A savestate is the whole machine as a list of words, and this is the
 * thing that counts through it.
 *
 * The machine is stopped first — the 6502 at an instruction boundary,
 * the soft CPU halted at its debug port — and then nothing is running
 * that could move a byte underneath the count. So the engine takes the
 * machine's own bus, which the soft CPU has stopped using, and reads
 * the memories through the same windows the firmware would. Nothing
 * marshals anything; nothing is copied twice.
 *
 * It is deliberately slow. A word costs a bus access and sometimes four,
 * and every access waits for the window to say it is ready. The only
 * deadline anywhere is at the far end, where the host asks for a word
 * every eighty-eight of its own clocks, and one word held ready ahead
 * of a sequential reader covers that with room to spare.
 *
 * Two things are not on the bus and are fetched their own way: the soft
 * CPU's memory, which rv_soc decodes internally, and the state that
 * lives in flops rather than memory — the 6502's registers, the VIA's
 * timers and pipelines, and the soft CPU's own registers, which come
 * out through its debug port a few instructions at a time.
 */

module sst_engine
    import rp6502_pkg::*;
(
    input logic clk_sys,
    input logic rst_n,

    /* Held for as long as a savestate is being made. */
    input logic sst_save,

    /* The machine, stopped. */
    output logic sst_engine_freeze,
    input logic frozen,
    output logic sst_engine_dbg_halt,
    input logic dbg_halted,

    /* Ready means the blob can be read; the word at rd_idx stands on
     * sst_engine_rdata once sst_engine_rvalid is high, and holds until
     * a different index is asked for. */
    output logic sst_engine_ready,
    input logic [17:0] rd_idx,
    input logic rd_req,
    output logic [31:0] sst_engine_rdata,
    output logic sst_engine_rvalid,

    /* The machine's bus, which this drives while it holds the machine. */
    output logic sst_engine_bus_own,
    output logic sst_engine_bus_pend,
    output logic sst_engine_bus_stb,
    output logic [31:0] sst_engine_bus_addr,
    input logic bus_rdy,
    input logic [31:0] bus_rdata,

    /* The soft CPU's memory, which is not on that bus. */
    output logic sst_engine_tcm_sel,
    output logic [14:0] sst_engine_tcm_addr,
    input logic [31:0] tcm_rdata,

    /* The flops. */
    output logic sst_engine_st_via,
    output logic [2:0] sst_engine_st_idx,
    input logic [31:0] st_rdata,

    /* The soft CPU's registers, through its debug port. */
    output logic [31:0] sst_engine_dbg_instr,
    output logic sst_engine_dbg_instr_vld,
    input logic dbg_instr_rdy,
    input logic dbg_ebreak,
    input logic [31:0] dbg_data0,
    input logic dbg_data0_wen
);

    /* The blob, in words. Nothing here is a size in bytes: the host
     * reads words and every region is a whole number of them. */
    localparam int W_HDR = 16;
    localparam int W_STATE = 64;
    localparam int W_REGS = 256;
    localparam int W_SRAM = 16384;
    localparam int W_XRAM = 16384;
    localparam int W_CELLS = 15360;
    localparam int W_TCM = 24576;
    localparam int W_END = 4;

    localparam int B_HDR = 0;
    localparam int B_STATE = B_HDR + W_HDR;
    localparam int B_REGS = B_STATE + W_STATE;
    localparam int B_SRAM = B_REGS + W_REGS;
    localparam int B_XRAM = B_SRAM + W_SRAM;
    localparam int B_CELLS = B_XRAM + W_XRAM;
    localparam int B_TCM = B_CELLS + W_CELLS;
    localparam int B_END = B_TCM + W_TCM;
    localparam int W_TOTAL = B_END + W_END;

    localparam logic [31:0] SST_MAGIC = 32'h52365353;      // "R6SS"
    localparam logic [31:0] SST_END_MAGIC = 32'h52365345;  // "R6SE"
    localparam logic [31:0] SST_VERSION = 32'd1;

    /* Where each window lives, as the machine's own addresses. */
    localparam logic [31:0] A_REGS = 32'h2000_0000;
    localparam logic [31:0] A_SRAM = 32'h1000_0000;
    localparam logic [31:0] A_XRAM = 32'h3000_0000;
    localparam logic [31:0] A_CELLS = 32'h5000_0000;

    /* The soft CPU's state, once it has been asked for it: thirty-one
     * registers and the program counter it will start from. */
    localparam int RV_WORDS = 32;
    logic [31:0] rvreg[RV_WORDS];
    logic [4:0] rv_n;

    localparam logic [11:0] CSR_DMDATA0 = 12'hBFF;
    localparam logic [11:0] CSR_DPC = 12'h7B1;
    localparam logic [31:0] I_EBREAK = 32'h00100073;

    /* csrw dmdata0, xN, which is csrrw with x0 for a destination: the
     * register lands on the debug port and nothing is read back. */
    function automatic logic [31:0] i_reg_out(input logic [4:0] n);
        return {CSR_DMDATA0, n, 3'b001, 5'd0, 7'b1110011};
    endfunction
    /* csrr xN, csr, which is csrrs with x0 for a source: the CSR is
     * read and not written. */
    function automatic logic [31:0] i_csr_read(input logic [11:0] csr,
                                               input logic [4:0] n);
        return {csr, 5'd0, 3'b010, n, 7'b1110011};
    endfunction

    typedef enum logic [3:0] {
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
        S_READ,
        S_READ_WAIT
    } state_t;
    state_t state;

    /* What the core last put on the debug port. Latched on the write
     * enable, because the value only stands while that is asserted --
     * by the time the ebreak says the instruction retired, the port is
     * carrying nothing. */
    logic [31:0] data0_q;

    /* The spill walks x1..x31 and then the program counter, which needs
     * a register to travel through and uses the one just saved. */
    logic spill_dpc;
    logic [1:0] spill_step;
    logic [31:0] spill_instr;

    /* A read in flight. */
    logic [17:0] hold_idx;
    logic [31:0] hold;
    logic hold_valid;
    logic [1:0] byte_n;
    logic [23:0] acc;
    logic bytewise;
    logic [31:0] region_addr;
    logic [17:0] off;

    always_comb begin
        sst_engine_freeze = state != S_IDLE;
        sst_engine_dbg_halt = state != S_IDLE;
        sst_engine_ready = state == S_READY || state == S_READ
            || state == S_READ_WAIT;
        sst_engine_rdata = hold;
        sst_engine_rvalid = hold_valid && hold_idx == rd_idx;
        sst_engine_bus_own = state != S_IDLE;
        sst_engine_dbg_instr = spill_instr;
        sst_engine_dbg_instr_vld = state == S_SPILL_ARM
            || state == S_SPILL_ISSUE || state == S_SPILL_BRK_ARM
            || state == S_SPILL_BRK;
    end

    /* Which window answers this word, and where in it. Byte windows
     * cost four accesses and the word is assembled with the lowest
     * address in the most significant byte, which is the order the
     * staging store hands the blob back in. */
    always_comb begin
        bytewise = 1'b0;
        region_addr = A_REGS;
        off = '0;
        if (hold_idx >= 18'(B_TCM)) off = hold_idx - 18'(B_TCM);
        else if (hold_idx >= 18'(B_CELLS)) begin
            off = hold_idx - 18'(B_CELLS);
            region_addr = A_CELLS;
        end else if (hold_idx >= 18'(B_XRAM)) begin
            off = hold_idx - 18'(B_XRAM);
            region_addr = A_XRAM;
            bytewise = 1'b1;
        end else if (hold_idx >= 18'(B_SRAM)) begin
            off = hold_idx - 18'(B_SRAM);
            region_addr = A_SRAM;
            bytewise = 1'b1;
        end else if (hold_idx >= 18'(B_REGS)) begin
            off = hold_idx - 18'(B_REGS);
            region_addr = A_REGS;
        end
    end

    logic on_bus, on_tcm;
    always_comb begin
        on_tcm = hold_idx >= 18'(B_TCM) && hold_idx < 18'(B_END);
        on_bus = hold_idx >= 18'(B_REGS) && hold_idx < 18'(B_TCM);
        sst_engine_tcm_sel = on_tcm && (state == S_READ
            || state == S_READ_WAIT);
        sst_engine_tcm_addr = off[14:0];
        sst_engine_bus_addr = region_addr
            + (bytewise ? {12'd0, off, 2'd0} + {30'd0, byte_n}
                        : {12'd0, off, 2'd0});
        sst_engine_bus_pend = on_bus && state == S_READ;
        sst_engine_bus_stb = on_bus && state == S_READ && bus_rdy;
    end

    /* The flops, which answer combinationally and need no access. */
    logic [31:0] state_word;
    always_comb begin
        sst_engine_st_via = hold_idx >= 18'(B_STATE + 5);
        sst_engine_st_idx = sst_engine_st_via
            ? 3'(hold_idx - 18'(B_STATE + 5)) : 3'(hold_idx - 18'(B_STATE));
        state_word = st_rdata;
        if (hold_idx >= 18'(B_STATE + 12))
            state_word = rvreg[5'(hold_idx - 18'(B_STATE + 12))];
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
    logic [31:0] sum;
    always_comb begin
        case (hold_idx - 18'(B_END))
            18'd0: end_word = sum;
            18'd1: end_word = 32'(W_TOTAL);
            18'd2: end_word = SST_END_MAGIC;
            default: end_word = 32'd0;
        endcase
    end

    /* Everything that is not a memory answers at once. */
    logic direct;
    logic [31:0] direct_word;
    always_comb begin
        direct = !on_bus && !on_tcm;
        direct_word = hold_idx < 18'(B_STATE) ? hdr_word
            : (hold_idx < 18'(B_REGS) ? state_word : end_word);
    end

    logic [17:0] sum_next;

    always_ff @(posedge clk_sys) begin
        if (!rst_n) begin
            state <= S_IDLE;
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
            sum_next <= '0;
        end else begin
            if (dbg_data0_wen) data0_q <= dbg_data0;
            case (state)
                S_IDLE: begin
                    hold_valid <= 1'b0;
                    sum <= '0;
                    sum_next <= '0;
                    rv_n <= 5'd1;
                    spill_dpc <= 1'b0;
                    spill_step <= '0;
                    if (sst_save) state <= S_FREEZE;
                end

                /* Both halves of the machine, in either order; the
                 * blob is not touched until both have stopped. */
                S_FREEZE: if (frozen) state <= S_HALT;
                S_HALT: begin
                    if (dbg_halted) begin
                        spill_instr <= i_reg_out(rv_n);
                        state <= S_SPILL_ARM;
                    end
                end

                /* One register per pass: put it on the port, then an
                 * ebreak, which is the only thing that says the
                 * instruction before it retired.
                 *
                 * Ready falling is what says the core took it. Letting
                 * go the moment ready is seen instead drops every other
                 * instruction, because the core runs on a slower clock
                 * than this and half these cycles are not its. */
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
                        /* The program counter travels through the
                         * register that was just saved. */
                        spill_step <= 2'd1;
                        spill_instr <= i_reg_out(5'd31);
                        state <= S_SPILL_ARM;
                    end else if (spill_dpc) begin
                        rvreg[0] <= data0_q;
                        state <= S_READY;
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

                S_READY: begin
                    if (!sst_save) state <= S_IDLE;
                    else if (rd_req && !(hold_valid && hold_idx == rd_idx))
                    begin
                        hold_idx <= rd_idx;
                        hold_valid <= 1'b0;
                        byte_n <= '0;
                        acc <= '0;
                        state <= S_READ;
                    end
                end

                /* Pend, wait for the window, take one strobe, and read
                 * the answer the clock after. */
                S_READ:
                if (direct) begin
                    hold <= direct_word;
                    hold_valid <= 1'b1;
                    state <= S_READY;
                end else if (on_tcm) begin
                    /* Registered on the soft CPU's own clock, which is
                     * half this one, so the address stands for a few
                     * before the word is believed. */
                    byte_n <= byte_n + 2'd1;
                    if (byte_n == 2'd3) state <= S_READ_WAIT;
                end else if (bus_rdy) begin
                    state <= S_READ_WAIT;
                end

                S_READ_WAIT: begin
                    if (on_tcm) begin
                        hold <= tcm_rdata;
                        hold_valid <= 1'b1;
                        state <= S_READY;
                    end else if (bytewise) begin
                        acc <= {acc[15:0], bus_rdata[7:0]};
                        if (byte_n == 2'd3) begin
                            hold <= {acc, bus_rdata[7:0]};
                            hold_valid <= 1'b1;
                            state <= S_READY;
                        end else begin
                            byte_n <= byte_n + 2'd1;
                            state <= S_READ;
                        end
                    end else begin
                        hold <= bus_rdata;
                        hold_valid <= 1'b1;
                        state <= S_READY;
                    end
                end

                default: state <= S_IDLE;
            endcase

            /* The running sum only follows a reader that walks the blob
             * in order, which is what the host does; the trailer says
             * what it saw so a blob served any other way is caught on
             * the way back in. */
            if (hold_valid && hold_idx == sum_next
                && sum_next < 18'(B_END)) begin
                sum <= {sum[30:0], sum[31]} + hold;
                sum_next <= sum_next + 18'd1;
            end
        end
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_sst_engine;
    always_comb unused_sst_engine = ^bus_rdata[31:8];
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
