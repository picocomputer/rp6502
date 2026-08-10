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

    /* Held for as long as a savestate is being made, and asked for on
     * the host's clock rather than this one, so it lands on two flops
     * before anything acts on it. The read request beside it is the
     * same signal and takes the same path. */
    input logic sst_save,

    /* A load runs the other way and needs nothing from the host: the
     * blob is already in the staging store, put there by the ordinary
     * bridge writes that carried it, so the engine reads it back
     * through the machine's own window and writes it where it goes. */
    input logic sst_load,
    output logic sst_engine_load_done,
    /* Refused, and nothing written: the machine is exactly as it was. */
    output logic sst_engine_load_err,

    /* The machine, stopped. */
    output logic sst_engine_freeze,
    input logic frozen,
    output logic sst_engine_dbg_halt,
    input logic dbg_halted,

    /* Ready means the blob can be read; the word at rd_idx stands on
     * sst_engine_rdata once sst_engine_rvalid is high, and holds until
     * a different index is asked for. */
    output logic sst_engine_ready,
    /* The index is data, and the toggle beside it is what says the data
     * has stopped moving. Nothing here looks at rd_idx until three
     * flops after that toggle changed -- a state machine that branched
     * on the raw crossing could take a transition on a value made of
     * bits from two different indices, which is a value that never
     * existed on either side. */
    input logic [17:0] rd_idx,
    input logic rd_t,
    output logic [31:0] sst_engine_rdata,
    output logic sst_engine_rvalid,

    /* The machine's bus, which this drives while it holds the machine. */
    output logic sst_engine_bus_own,
    output logic sst_engine_bus_pend,
    output logic sst_engine_bus_stb,
    output logic [31:0] sst_engine_bus_addr,
    output logic sst_engine_bus_we,
    output logic [31:0] sst_engine_bus_wdata,
    output logic [3:0] sst_engine_bus_wstrb,
    input logic bus_rdy,
    input logic [31:0] bus_rdata,

    /* The soft CPU's memory, which is not on that bus. */
    output logic sst_engine_tcm_sel,
    output logic [14:0] sst_engine_tcm_addr,
    output logic sst_engine_tcm_we,
    output logic [31:0] sst_engine_tcm_wdata,
    input logic [31:0] tcm_rdata,

    /* The flops: the machine's own, the 6502's and the VIA's. */
    output logic [1:0] sst_engine_st_sel,
    output logic [2:0] sst_engine_st_idx,
    input logic [31:0] st_rdata,
    output logic sst_engine_st_we,
    output logic [31:0] sst_engine_st_wdata,

    /* The soft CPU's registers, through its debug port. */
    /* What the core reads back out of dmdata0 when a register is put
     * in, and the release that starts it running again. */
    output logic [31:0] sst_engine_dbg_data0,
    output logic sst_engine_dbg_resume,
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
    /* The scanline program: two thousand entries of four words, which
     * the render reads and nothing else could until now. */
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

    localparam logic [31:0] SST_MAGIC = 32'h52365353;      // "R6SS"
    localparam logic [31:0] SST_END_MAGIC = 32'h52365345;  // "R6SE"
    localparam logic [31:0] SST_VERSION = 32'd1;

    /* Where each window lives, as the machine's own addresses. */
    localparam logic [31:0] A_REGS = 32'h2000_0000;
    localparam logic [31:0] A_SRAM = 32'h1000_0000;
    localparam logic [31:0] A_XRAM = 32'h3000_0000;
    localparam logic [31:0] A_CELLS = 32'h5000_0000;
    localparam logic [31:0] A_XPROG = 32'h5002_0000;
    /* The blob, where the host left it, seen through the machine's
     * window onto the staging store. */
    localparam logic [31:0] A_STAGE = 32'h63F0_0000;

    /* The one word of the regs window that cannot be read: word 16 is
     * the console's outgoing byte and reading it takes it off the
     * queue. A savestate that read it would eat a character every time
     * one was made, so it is not read and not written -- the queue is
     * one of the things a sleep is documented to lose. */
    localparam int REGS_HOLE = 16;

    /* The state page, in the order a restore has to put it back. What
     * the machine is -- whether the 6502 is out of reset at all -- comes
     * before what it holds, because registers jammed into a core that is
     * still in reset are overwritten by the reset the next clock, so the
     * machine's own words are the ones at zero. */
    localparam int ST_CPU = 4;
    localparam int ST_VIA = 9;
    localparam int ST_RV = 16;
    localparam logic [1:0] SEL_MACH = 2'd0;
    localparam logic [1:0] SEL_CPU = 2'd1;
    localparam logic [1:0] SEL_VIA = 2'd2;

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
    /* csrr xN, dmdata0: what the engine put on the port lands in the
     * register. csrrs with x0 for a source writes nothing back. */
    function automatic logic [31:0] i_reg_in(input logic [4:0] n);
        return {CSR_DMDATA0, 5'd0, 3'b010, n, 7'b1110011};
    endfunction
    /* csrw csr, xN, which is csrrw with x0 for a destination. */
    function automatic logic [31:0] i_csr_write(input logic [11:0] csr,
                                                input logic [4:0] n);
        return {csr, n, 3'b001, 5'd0, 7'b1110011};
    endfunction
    /* csrr xN, csr, which is csrrs with x0 for a source: the CSR is
     * read and not written. */
    function automatic logic [31:0] i_csr_read(input logic [11:0] csr,
                                               input logic [4:0] n);
        return {csr, 5'd0, 3'b010, n, 7'b1110011};
    endfunction

    typedef enum logic [4:0] {
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
        S_READ_WAIT,
        S_LD_FREEZE,
        S_LD_HALT,
        S_LD_FETCH,
        S_LD_FETCH_WAIT,
        S_LD_CHK,
        S_LD_BAD,
        S_LD_PUT,
        S_LD_PUT_WAIT,
        S_LD_PUT_DONE,
        S_LD_DONE,
        S_LD_ACK,
        S_INJ_ARM,
        S_INJ_ISSUE,
        S_INJ_TAKE,
        S_INJ_BRK_ARM,
        S_INJ_BRK,
        S_INJ_WAIT
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
    logic [17:0] off, dec_idx;

    /* Both of these cross to the host's clock, so both are flops rather
     * than the logic behind them. A comparator or an OR of state bits
     * glitches while its inputs settle, and a synchroniser fed from a
     * cone can latch a transient that was never a state the design was
     * in -- which is what the Design Assistant means by data that is
     * not correctly synchronised, as opposed to merely unsynchronised. */
    logic ready_q, rvalid_q, done_q;
    always_comb begin
        sst_engine_freeze = state != S_IDLE;
        sst_engine_dbg_halt = state != S_IDLE && state != S_LD_DONE
            && state != S_LD_ACK;
        sst_engine_load_done = done_q;
        sst_engine_load_err = ld_bad;
        sst_engine_ready = ready_q;
        sst_engine_rdata = hold;
        sst_engine_rvalid = rvalid_q;
        sst_engine_bus_own = state != S_IDLE;
        sst_engine_dbg_instr = spill_instr;
        sst_engine_dbg_instr_vld = state == S_SPILL_ARM
            || state == S_SPILL_ISSUE || state == S_SPILL_BRK_ARM
            || state == S_SPILL_BRK || state == S_INJ_ARM
            || state == S_INJ_ISSUE || state == S_INJ_BRK_ARM
            || state == S_INJ_BRK;
        /* The value stands on the port for as long as the instruction
         * that reads it is in flight. */
        sst_engine_dbg_data0 = inj_val;
        sst_engine_dbg_resume = state == S_LD_DONE;
    end

    /* Which window answers this word, and where in it. Byte windows
     * cost four accesses and the word is assembled with the lowest
     * address in the most significant byte, which is the order the
     * staging store hands the blob back in. */
    always_comb begin
        bytewise = 1'b0;
        region_addr = A_REGS;
        off = '0;
        dec_idx = ld_writing ? ld_idx : hold_idx;
        if (dec_idx >= 18'(B_TCM)) off = dec_idx - 18'(B_TCM);
        else if (dec_idx >= 18'(B_XPROG)) begin
            off = dec_idx - 18'(B_XPROG);
            region_addr = A_XPROG;
        end else if (dec_idx >= 18'(B_CELLS)) begin
            off = dec_idx - 18'(B_CELLS);
            region_addr = A_CELLS;
        end else if (dec_idx >= 18'(B_XRAM)) begin
            off = dec_idx - 18'(B_XRAM);
            region_addr = A_XRAM;
            bytewise = 1'b1;
        end else if (dec_idx >= 18'(B_SRAM)) begin
            off = dec_idx - 18'(B_SRAM);
            region_addr = A_SRAM;
            bytewise = 1'b1;
        end else if (dec_idx >= 18'(B_REGS)) begin
            off = dec_idx - 18'(B_REGS);
            region_addr = A_REGS;
        end
    end

    logic on_bus, on_tcm, on_hole;
    always_comb begin
        on_hole = dec_idx == 18'(B_REGS + REGS_HOLE);
        on_tcm = dec_idx >= 18'(B_TCM) && dec_idx < 18'(B_END);
        on_bus = dec_idx >= 18'(B_REGS) && dec_idx < 18'(B_TCM) && !on_hole;
        sst_engine_tcm_sel = on_tcm && (state == S_READ
            || state == S_READ_WAIT || state == S_LD_PUT
            || state == S_LD_PUT_WAIT);
        sst_engine_tcm_addr = off[14:0];
        sst_engine_tcm_we = on_tcm && state == S_LD_PUT;
        sst_engine_tcm_wdata = ld_word;
        sst_engine_bus_addr = (state == S_LD_FETCH
                || state == S_LD_FETCH_WAIT)
            ? A_STAGE + {12'd0, ld_idx, 2'd0} + {30'd0, byte_n}
            : region_addr + (bytewise ? {12'd0, off, 2'd0} + {30'd0, byte_n}
                                      : {12'd0, off, 2'd0});
        /* A fetch reads the store; a put writes wherever the index
         * says. Byte windows take one byte per access either way. */
        sst_engine_bus_we = state == S_LD_PUT;
        /* The byte windows take the addressed lane off whichever byte
         * the master offers, so a write puts the same byte on all four
         * and the window picks it up; a word window takes the word. */
        sst_engine_bus_wstrb = state == S_LD_PUT ? 4'b1111 : 4'b0000;
        sst_engine_bus_wdata = bytewise
            ? {4{ld_word[31 - {byte_n, 3'd0} -: 8]}} : ld_word;
        sst_engine_bus_pend = (on_bus && state == S_READ)
            || state == S_LD_FETCH || (on_bus && state == S_LD_PUT);
        sst_engine_bus_stb = bus_rdy && ((on_bus && state == S_READ)
            || state == S_LD_FETCH || (on_bus && state == S_LD_PUT));
    end

    /* The flops, which answer combinationally and need no access. Five
     * words of 6502 and seven of VIA, and the soft CPU's registers after
     * them -- those come from and go back through the debug port, so
     * they only pass through here. */
    logic [31:0] state_word;
    logic on_flops;
    logic [17:0] st_off;
    always_comb begin
        st_off = dec_idx - 18'(B_STATE);
        on_flops = dec_idx >= 18'(B_STATE) && dec_idx < 18'(B_STATE + ST_RV);
        if (st_off >= 18'(ST_VIA)) begin
            sst_engine_st_sel = SEL_VIA;
            sst_engine_st_idx = 3'(st_off - 18'(ST_VIA));
        end else if (st_off >= 18'(ST_CPU)) begin
            sst_engine_st_sel = SEL_CPU;
            sst_engine_st_idx = 3'(st_off - 18'(ST_CPU));
        end else begin
            sst_engine_st_sel = SEL_MACH;
            sst_engine_st_idx = 3'(st_off);
        end
        sst_engine_st_we = on_flops && state == S_LD_PUT;
        sst_engine_st_wdata = ld_word;
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
            : (hold_idx < 18'(B_REGS) ? state_word
               : (hold_idx >= 18'(B_END) ? end_word : 32'd0));
    end

    logic [17:0] sum_next;

    /* A load walks the blob in order rather than being asked for words:
     * four bytes out of the store, one word into whatever the index
     * says, and on to the next. */
    /* The inject walks the program counter first and the registers
     * after, because the counter needs one to travel through and
     * nothing may disturb a register once it is set. */
    logic [31:0] inj_val;
    logic inj_dpc;
    logic [1:0] inj_step;
    logic [17:0] ld_idx;
    logic [31:0] ld_word;
    logic ld_writing;

    /* The blob is read twice: once to see whether it is one, and once
     * to put it in. Nothing is written until the whole of it has been
     * added up and the trailer agrees, because a restore that got
     * halfway and found out is a machine that no longer exists. */
    logic ld_verify, ld_bad;
    logic [31:0] vsum;

    /* Named _s1 so the platform's standing rule cuts the arrival. */
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
            ready_q <= 1'b0;
            rvalid_q <= 1'b0;
            done_q <= 1'b0;
            save_s1 <= 1'b0;
            save_s2 <= 1'b0;
            load_s1 <= 1'b0;
            load_s2 <= 1'b0;
            ld_idx <= '0;
            ld_word <= '0;
            ld_writing <= 1'b0;
            ld_verify <= 1'b0;
            ld_bad <= 1'b0;
            vsum <= '0;
            inj_val <= '0;
            inj_dpc <= 1'b0;
            inj_step <= '0;
            idx_t1 <= 1'b0;
            idx_t2 <= 1'b0;
            idx_t3 <= 1'b0;
            req_idx <= '0;
            req_pending <= 1'b0;
        end else begin
            ready_q <= state == S_READY || state == S_READ
                || state == S_READ_WAIT;
            done_q <= state == S_LD_DONE || state == S_LD_ACK;
            rvalid_q <= hold_valid && hold_idx == req_idx;
            save_s1 <= sst_save;
            save_s2 <= save_s1;
            load_s1 <= sst_load;
            load_s2 <= load_s1;
            idx_t1 <= rd_t;
            idx_t2 <= idx_t1;
            idx_t3 <= idx_t2;
            /* A request is a thing that arrived, not a comparison
             * that happens to be true: with nothing held yet, asking
             * "is the held word the wrong one" is true of an index
             * nobody has asked for. */
            if (req_new) begin
                req_idx <= rd_idx;
                req_pending <= 1'b1;
            end
            if (dbg_data0_wen) data0_q <= dbg_data0;
            case (state)
                S_IDLE: begin
                    hold_valid <= 1'b0;
                    sum <= '0;
                    sum_next <= '0;
                    rv_n <= 5'd1;
                    spill_dpc <= 1'b0;
                    spill_step <= '0;
                    if (save_req) state <= S_FREEZE;
                    else if (load_req) state <= S_LD_FREEZE;
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

                /* A load: stop the machine, then walk the blob out of
                 * the store and into the machine a word at a time. */
                S_LD_FREEZE: if (frozen) state <= S_LD_HALT;
                S_LD_HALT:
                if (dbg_halted) begin
                    ld_idx <= '0;
                    byte_n <= '0;
                    acc <= '0;
                    vsum <= '0;
                    ld_bad <= 1'b0;
                    ld_verify <= 1'b1;
                    state <= S_LD_FETCH;
                end

                S_LD_FETCH: if (bus_rdy) state <= S_LD_FETCH_WAIT;
                S_LD_FETCH_WAIT: begin
                    acc <= {acc[15:0], bus_rdata[7:0]};
                    if (byte_n == 2'd3) begin
                        ld_word <= {acc, bus_rdata[7:0]};
                        byte_n <= '0;
                        state <= ld_verify ? S_LD_CHK : S_LD_PUT;
                    end else begin
                        byte_n <= byte_n + 2'd1;
                        state <= S_LD_FETCH;
                    end
                end

                /* The same running sum the save wrote the blob with, so
                 * a blob served out of order fails here rather than
                 * being taken for the machine it is not. */
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
                        /* Whole and consistent. The state page first,
                         * because the machine's own flops decide what
                         * the rest of it means. */
                        ld_idx <= 18'(B_STATE);
                        ld_verify <= 1'b0;
                        ld_writing <= 1'b1;
                        state <= S_LD_FETCH;
                    end else begin
                        ld_idx <= ld_idx + 18'd1;
                        state <= S_LD_FETCH;
                    end
                end

                /* Not a blob this machine can use. Nothing has been
                 * written, so letting the soft CPU go puts it back
                 * exactly where the halt found it. */
                S_LD_BAD: begin
                    ld_bad <= 1'b1;
                    ld_verify <= 1'b0;
                    state <= S_LD_DONE;
                end

                S_LD_PUT: begin
                    /* The soft CPU's registers are not memory: they are
                     * kept here until the walk is finished and then put
                     * back through the debug port. */
                    if (ld_idx >= 18'(B_STATE + ST_RV)
                        && ld_idx < 18'(B_STATE + ST_RV + RV_WORDS))
                        rvreg[5'(ld_idx - 18'(B_STATE + ST_RV))] <= ld_word;
                    /* The soft CPU's memory and one of the machine's
                     * own words are written on its clock, which is half
                     * this one, so the word is held out for four of
                     * these -- two of its own -- rather than offered
                     * for one and missed depending on which half of its
                     * period the offer fell in. The flops that are on
                     * this clock take the same word four times and are
                     * none the worse for it. */
                    if (on_tcm || on_flops) begin
                        byte_n <= byte_n + 2'd1;
                        if (byte_n == 2'd3) state <= S_LD_PUT_WAIT;
                    end else if (bus_rdy) state <= S_LD_PUT_WAIT;
                    else if (!on_bus) state <= S_LD_PUT_WAIT;
                end
                S_LD_PUT_WAIT:
                if (!on_tcm && bytewise && byte_n != 2'd3) begin
                    byte_n <= byte_n + 2'd1;
                    state <= S_LD_PUT;
                end else begin
                    byte_n <= '0;
                    if (ld_idx == 18'(B_END - 1)) begin
                        ld_writing <= 1'b0;
                        state <= S_LD_PUT_DONE;
                    end else begin
                        ld_idx <= ld_idx + 18'd1;
                        state <= S_LD_FETCH;
                    end
                end

                /* Everything is in memory; what is left is the soft
                 * CPU's own registers, which are not. The counter goes
                 * first through x31, then x1 upward, so the register it
                 * borrowed is put right along with the rest. */
                S_LD_PUT_DONE: begin
                    ld_bad <= 1'b0;
                    inj_dpc <= 1'b1;
                    inj_step <= 2'd0;
                    inj_val <= rvreg[0];
                    rv_n <= 5'd31;
                    spill_instr <= i_reg_in(5'd31);
                    state <= S_INJ_ARM;
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
                        state <= S_LD_DONE;
                    end else begin
                        rv_n <= rv_n + 5'd1;
                        inj_val <= rvreg[5'(rv_n + 5'd1)];
                        spill_instr <= i_reg_in(rv_n + 5'd1);
                        state <= S_INJ_ARM;
                    end
                end

                /* Released, and held released until the core is seen
                 * to be running. The soft CPU's clock is half this one
                 * and the request that ends the load is cleared by the
                 * firmware on it, so a resume that let go on the
                 * request would be asking the core to answer before it
                 * has had a clock edge to hear the question in. */
                S_LD_DONE: if (!dbg_halted) state <= S_LD_ACK;

                /* Leaves on the same signal it arrived on. Waiting on
                 * the raw level instead lets go two clocks before the
                 * synchronised copy does, and idle starts the load
                 * again on the stale one. The 6502 stays stopped for
                 * all of it: the firmware has fabric to put back that
                 * no blob carries, and it does that before the machine
                 * it belongs to takes another cycle. */
                S_LD_ACK: if (!load_req) state <= S_IDLE;

                S_READY: begin
                    if (!save_req) state <= S_IDLE;
                    else if (save_req && req_pending) begin
                        req_pending <= 1'b0;
                        hold_idx <= req_idx;
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
