/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The staging store. Its clients are the soft CPU, the bridge and
 * pocket_sst, never the 6502, and the floor they need is full-speed USB,
 * 1.5 MB/s. An access is ten clocks and more, so a clock spent here to
 * take arithmetic off a path costs nothing that matters.
 *
 * There are three clients. The write client moves one 16-bit word, or
 * the bytes of it w_mask names, bit 0 the low byte; DQM masks the rest.
 * The read client and the line client move a line, eight words that
 * share a row: a line's address is the word address without its low
 * three bits, and bits [16k+15:16k] of the line are word k. A line is
 * eight READs or eight WRITEs on eight clocks, with the chip still at
 * a burst length of one.
 *
 * The read client is a look-ahead. rd_addr counts pairs of words, so
 * it is a byte address without its low two bits. A visit reads the
 * line rd_addr is in and holds it, and pocket_sdram_rvalid is high
 * while the held line is that one, with pocket_sdram_rword the pair
 * at rd_addr, the lower word in its low half. The reads that follow in
 * the same line are answered with no visit.
 *
 * The held line is never older than the chip. A read runs from S_IDLE
 * to S_IDLE with nothing taken in between, and the line becomes valid
 * on the clock its last word lands. Writes are taken only in S_IDLE.
 * Any write of the write client drops the line on the clock it is
 * taken, whatever its address, and so does a line written over it.
 * That clock is before the WRITE reaches the chip, so the held line
 * never answers after a write it does not have.
 *
 * line_pend stays high, with line_we, line_addr and line_wdata stable,
 * until pocket_sdram_line_done rises. pocket_sdram_line_done falls
 * after line_pend does. pocket_sdram_line_rdata holds a line that was
 * read until the next line is taken.
 */

module pocket_sdram #(
    parameter bit SELF_REFRESH = 1'b1,
    parameter bit CLOSE_IDLE_ROWS = 1'b1
) (
    input logic clk,
    /* The PLL's lock, synchronised to clk. The clock is not to be
     * trusted before it, so the controller is held in S_BOOT and the
     * chip's 200 us of power-up wait is counted from the lock. */
    input logic pll_locked,

    input logic rd_pend,
    input logic [23:0] rd_addr,
    output logic [31:0] pocket_sdram_rword,
    output logic pocket_sdram_rvalid,

    input logic w_avail,
    input logic [24:0] w_addr,
    input logic [15:0] w_data,
    input logic [1:0] w_mask,
    output logic pocket_sdram_wtake,

    input logic line_pend,
    input logic line_we,
    input logic [21:0] line_addr,
    input logic [127:0] line_wdata,
    output logic [127:0] pocket_sdram_line_rdata,
    output logic pocket_sdram_line_done,

    output logic pocket_sdram_ready,

    output logic dram_cke,
    output logic [12:0] dram_a,
    output logic [1:0] dram_ba,
    output logic [1:0] dram_dqm,
    output logic dram_ras_n,
    output logic dram_cas_n,
    output logic dram_we_n,
    output logic [15:0] dram_dq_out,
    output logic dram_dq_oe,
    input logic [15:0] dram_dq_in
);

    /* INIT_WAIT is 200 us at 50.4 MHz. */
    localparam int INIT_WAIT = 10100;
    localparam logic [12:0] MODE_CL2_BL1 = 13'b000_0_00_010_0_000;
    /* BA = 2'b10 selects the extended mode register, and its A2:A0 field
     * is Partial Array Self Refresh (PASR). PASR sets which banks are
     * refreshed during self refresh, and with PASR at zero all four are
     * refreshed. The register powers up in an unknown state, so it has to
     * be written before the first self refresh. */
    localparam logic [12:0] EMODE_PASR_ALL = 13'b000_0_00_000_0_000;
    /* The datasheet requires 8192 auto refreshes every 64 ms, which is
     * one every 7812.5 ns, so REFRESH_EVERY has to be at most 393, since
     * 393 clocks at 50.4 MHz is 7797 ns and 394 is 7817 ns. */
    localparam int REFRESH_EVERY = 390;

    localparam int IDLE_CLOSE = 64;  /* 1.3 us */
    localparam int IDLE_SREF = 256;  /* 5.1 us */

    /* The datasheet requires self refresh to last at least tRAS, 48 ns
     * or three clocks. A request can arrive on the clock after entry, so
     * without this floor the chip could enter and leave self refresh in
     * one clock. Eight clocks also covers tRFC, five clocks, after the
     * AUTO REFRESH command that is issued on entry. */
    localparam int SREF_MIN = 8;

    typedef enum logic [4:0] {
        S_BOOT, S_PALL, S_REF0, S_REF1, S_MRS, S_EMRS,
        S_IDLE, S_OPEN, S_REFRESH, S_PALL_R, S_PALL_I, S_PRE, S_ACT,
        S_READ, S_WRITE, S_DONE, S_SREF_ENT, S_SREF, S_WAIT
    } state_t;
    state_t state, after;

    logic [14:0] wait_cnt;
    logic [9:0] ref_cnt;
    logic [1:0] ref_due;
    logic [9:0] idle_cnt;
    logic [3:0] sref_cnt;

    /* op_is_line marks the line client's work. A read is a line for
     * either client, and op_addr[2:0] counts a line's eight words. */
    logic op_is_read;
    logic op_is_line;
    logic [24:0] op_addr;
    logic [15:0] op_wdata;
    logic [1:0] op_wmask;

    logic [127:0] held;
    logic [21:0] held_addr;
    logic held_valid;
    always_comb begin
        pocket_sdram_rvalid = held_valid && held_addr == rd_addr[23:2];
        pocket_sdram_rword = held[{rd_addr[1:0], 5'd0} +: 32];
    end

    /* The chip's clock is this clock inverted, so the chip samples a
     * command half a clock after the clock that issues it. With CL2
     * the data of a READ is good on the chip's second clock after the
     * one that samples the READ, which is the falling edge two and a
     * half clocks after its S_READ, and with reads back to back it is
     * good at no other edge. dq_cap takes the bus on every falling
     * edge, and rd_pipe[2] marks the clock, three after an S_READ,
     * that takes that READ's word from dq_cap. */
    logic [15:0] dq_cap;
    logic [2:0] rd_pipe;
    initial dq_cap = '0;
    always_ff @(negedge clk) dq_cap <= dram_dq_in;

    logic [12:0] row_open[4];
    logic bank_active[4];
    logic any_active;
    always_comb any_active = bank_active[0] || bank_active[1]
                          || bank_active[2] || bank_active[3];

    /* Decided from the registered address a clock after the request is
     * taken, so the row compare starts at a register and not at the
     * clients' address mux. */
    logic [1:0] req_bank;
    logic [12:0] req_row;
    logic req_hit, req_needs_pre;
    always_comb begin
        req_bank = op_addr[24:23];
        req_row  = op_addr[22:10];
        req_hit = bank_active[req_bank] && row_open[req_bank] == req_row;
        req_needs_pre = bank_active[req_bank] && !req_hit;
    end


    initial begin
        state = S_BOOT;
        after = S_IDLE;
        wait_cnt = 15'(INIT_WAIT);
        ref_cnt = '0;
        ref_due = '0;
        idle_cnt = '0;
        sref_cnt = '0;
        dram_cke = 1'b1;
        op_is_read = 1'b0;
        op_is_line = 1'b0;
        op_addr = '0;
        op_wdata = '0;
        op_wmask = '0;
        held = '0;
        held_addr = '0;
        held_valid = 1'b0;
        rd_pipe = '0;
        for (int b = 0; b < 4; b++) begin
            row_open[b] = '0;
            bank_active[b] = 1'b0;
        end
        pocket_sdram_wtake = 1'b0;
        pocket_sdram_line_rdata = '0;
        pocket_sdram_line_done = 1'b0;
        pocket_sdram_ready = 1'b0;
        dram_a = '0;
        dram_ba = '0;
        dram_ras_n = 1'b1;
        dram_cas_n = 1'b1;
        dram_we_n = 1'b1;
        dram_dq_out = '0;
        dram_dq_oe = 1'b0;
        dram_dqm = '0;
    end
    always_ff @(posedge clk) begin
        {dram_ras_n, dram_cas_n, dram_we_n} <= 3'b111;
        dram_dq_oe <= 1'b0;
        dram_dqm <= 2'b00;
        pocket_sdram_wtake <= 1'b0;

        if (pocket_sdram_ready && state != S_SREF) begin
            if (ref_cnt == 10'(REFRESH_EVERY - 1)) begin
                ref_cnt <= '0;
                if (ref_due != 2'd3)
                    ref_due <= ref_due + 2'd1;
            end else begin
                ref_cnt <= ref_cnt + 10'd1;
            end
        end

        /* A line's words arrive lowest first and shift down, so the
         * eighth leaves word 0 in the low bits. */
        rd_pipe <= {rd_pipe[1:0], 1'b0};
        if (rd_pipe[2]) begin
            if (op_is_line)
                pocket_sdram_line_rdata <=
                    {dq_cap, pocket_sdram_line_rdata[127:16]};
            else
                held <= {dq_cap, held[127:16]};
        end

        if (!line_pend)
            pocket_sdram_line_done <= 1'b0;

        case (state)
            S_BOOT: begin
                if (wait_cnt == '0)
                    state <= S_PALL;
                else
                    wait_cnt <= wait_cnt - 15'd1;
            end
            S_PALL: begin
                {dram_ras_n, dram_cas_n, dram_we_n} <= 3'b010;
                dram_a <= 13'h400; /* A10: all banks */
                wait_cnt <= 15'd3;
                after <= S_REF0;
                state <= S_WAIT;
            end
            S_REF0: begin
                {dram_ras_n, dram_cas_n, dram_we_n} <= 3'b001;
                wait_cnt <= 15'd8;
                after <= S_REF1;
                state <= S_WAIT;
            end
            S_REF1: begin
                {dram_ras_n, dram_cas_n, dram_we_n} <= 3'b001;
                wait_cnt <= 15'd8;
                after <= S_MRS;
                state <= S_WAIT;
            end
            S_MRS: begin
                {dram_ras_n, dram_cas_n, dram_we_n} <= 3'b000;
                dram_a <= MODE_CL2_BL1;
                dram_ba <= 2'b00;
                wait_cnt <= 15'd2; /* tMRD */
                after <= S_EMRS;
                state <= S_WAIT;
            end
            S_EMRS: begin
                {dram_ras_n, dram_cas_n, dram_we_n} <= 3'b000;
                dram_a <= EMODE_PASR_ALL;
                dram_ba <= 2'b10;
                wait_cnt <= 15'd2; /* tMRD */
                after <= S_IDLE;
                state <= S_WAIT;
                pocket_sdram_ready <= 1'b1;
            end
            S_IDLE: begin
                /* One refresh due waits for a read that is waiting, since
                 * the host gives the savestate's reads about a microsecond
                 * each. A second one due goes first, so none is lost. */
                if (ref_due[1] || (ref_due[0]
                        && !(rd_pend && !pocket_sdram_rvalid))) begin
                    idle_cnt <= '0;
                    state <= any_active ? S_PALL_R : S_REFRESH;
                end else if (w_avail) begin
                    /* A write goes before a read or a line. The host
                     * sends writes at a rate it sets, their queue is
                     * short and the bridge cannot make the host wait.
                     * The soft CPU asks for reads and lines back to
                     * back, so either taken first would overflow the
                     * queue. */
                    idle_cnt <= '0;
                    op_is_read <= 1'b0;
                    op_is_line <= 1'b0;
                    op_addr <= w_addr;
                    op_wdata <= w_data;
                    op_wmask <= w_mask;
                    pocket_sdram_wtake <= 1'b1;
                    held_valid <= 1'b0;
                    state <= S_OPEN;
                end else if (rd_pend && !pocket_sdram_rvalid) begin
                    idle_cnt <= '0;
                    op_is_read <= 1'b1;
                    op_is_line <= 1'b0;
                    op_addr <= {rd_addr[23:2], 3'b000};
                    held_addr <= rd_addr[23:2];
                    held_valid <= 1'b0;
                    state <= S_OPEN;
                end else if (line_pend && !pocket_sdram_line_done) begin
                    idle_cnt <= '0;
                    op_is_read <= !line_we;
                    op_is_line <= 1'b1;
                    op_addr <= {line_addr, 3'b000};
                    /* A line to write waits in the register that a
                     * line read fills, and leaves it a word a clock. */
                    if (line_we) begin
                        pocket_sdram_line_rdata <= line_wdata;
                        if (held_addr == line_addr)
                            held_valid <= 1'b0;
                    end
                    state <= S_OPEN;
                end else begin
                    if (!rd_pend && !w_avail && !line_pend)
                        idle_cnt <= idle_cnt + 10'd1;
                    if (CLOSE_IDLE_ROWS && idle_cnt == 10'(IDLE_CLOSE)
                        && any_active)
                        state <= S_PALL_I;
                    else if (SELF_REFRESH && idle_cnt == 10'(IDLE_SREF)
                             && !any_active)
                        state <= S_SREF_ENT;
                end
            end
            S_OPEN:
                state <= req_hit ? (op_is_read ? S_READ : S_WRITE)
                       : (req_needs_pre ? S_PRE : S_ACT);
            S_PALL_R: begin
                {dram_ras_n, dram_cas_n, dram_we_n} <= 3'b010;
                dram_a <= 13'h400; /* A10: all banks */
                for (int b = 0; b < 4; b++)
                    bank_active[b] <= 1'b0;
                wait_cnt <= 15'd1; /* tRP */
                after <= S_REFRESH;
                state <= S_WAIT;
            end
            S_PALL_I: begin
                {dram_ras_n, dram_cas_n, dram_we_n} <= 3'b010;
                dram_a <= 13'h400;
                for (int b = 0; b < 4; b++)
                    bank_active[b] <= 1'b0;
                wait_cnt <= 15'd1; /* tRP */
                after <= S_IDLE;
                state <= S_WAIT;
            end
            /* Self refresh is entered by an AUTO REFRESH issued with CKE
             * low, which requires every bank to be precharged. */
            S_SREF_ENT: begin
                {dram_ras_n, dram_cas_n, dram_we_n} <= 3'b001;
                dram_cke <= 1'b0;
                sref_cnt <= '0;
                state <= S_SREF;
            end
            S_SREF:
                if (sref_cnt != 4'(SREF_MIN))
                    sref_cnt <= sref_cnt + 4'd1;
                else if (rd_pend || w_avail || line_pend) begin
                    dram_cke <= 1'b1;
                    /* Five clocks cover tXSR, 80 ns. The chip refreshes
                     * itself during self refresh, so the refresh count
                     * starts over rather than resuming. */
                    wait_cnt <= 15'd5;
                    after <= S_IDLE;
                    state <= S_WAIT;
                    ref_cnt <= '0;
                    ref_due <= '0;
                    idle_cnt <= '0;
                end
            S_REFRESH: begin
                {dram_ras_n, dram_cas_n, dram_we_n} <= 3'b001;
                ref_due <= ref_due - 2'd1;
                wait_cnt <= 15'd8;
                after <= S_IDLE;
                state <= S_WAIT;
            end
            /* No counter enforces tRAS, three clocks, because every
             * ACTIVE is followed by its READs or WRITEs before S_IDLE
             * runs again, which puts at least eight clocks between an
             * ACTIVE and any precharge. */
            S_PRE: begin
                {dram_ras_n, dram_cas_n, dram_we_n} <= 3'b010;
                dram_ba <= op_addr[24:23];
                dram_a <= 13'h000; /* A10 low: this bank only */
                bank_active[op_addr[24:23]] <= 1'b0;
                wait_cnt <= 15'd1; /* tRP */
                after <= S_ACT;
                state <= S_WAIT;
            end
            S_ACT: begin
                {dram_ras_n, dram_cas_n, dram_we_n} <= 3'b011;
                dram_ba <= op_addr[24:23];
                dram_a <= op_addr[22:10];
                row_open[op_addr[24:23]] <= op_addr[22:10];
                bank_active[op_addr[24:23]] <= 1'b1;
                wait_cnt <= 15'd1; /* tRCD */
                after <= op_is_read ? S_READ : S_WRITE;
                state <= S_WAIT;
            end
            /* A10 low in a READ or WRITE leaves auto-precharge off, so
             * the row stays open after the access.
             *
             * S_READ runs eight times, and S_WRITE once for a word and
             * eight times for a line. The word of a READ is taken three
             * clocks after its S_READ, so S_DONE runs on the clock that
             * takes the last one. The chip leaves the bus before that clock,
             * and S_IDLE follows S_DONE, so the earliest WRITE drives
             * the bus three clocks after the chip has left it. */
            S_READ: begin
                {dram_ras_n, dram_cas_n, dram_we_n} <= 3'b101;
                dram_ba <= op_addr[24:23];
                dram_a <= {3'b000, op_addr[9:0]};
                rd_pipe[0] <= 1'b1;
                if (op_addr[2:0] != 3'd7) begin
                    op_addr[2:0] <= op_addr[2:0] + 3'd1;
                end else begin
                    wait_cnt <= 15'd1;
                    after <= S_DONE;
                    state <= S_WAIT;
                end
            end
            S_WRITE: begin
                {dram_ras_n, dram_cas_n, dram_we_n} <= 3'b100;
                dram_ba <= op_addr[24:23];
                dram_a <= {3'b000, op_addr[9:0]};
                dram_dq_out <= op_is_line ? pocket_sdram_line_rdata[15:0]
                                          : op_wdata;
                dram_dq_oe <= 1'b1;
                dram_dqm <= op_is_line ? 2'b00 : ~op_wmask;
                if (op_is_line)
                    pocket_sdram_line_rdata[111:0] <=
                        pocket_sdram_line_rdata[127:16];
                if (op_is_line && op_addr[2:0] != 3'd7) begin
                    op_addr[2:0] <= op_addr[2:0] + 3'd1;
                end else begin
                    wait_cnt <= 15'd1; /* tWR, with S_DONE */
                    after <= S_DONE;
                    state <= S_WAIT;
                end
            end
            S_DONE: begin
                if (op_is_line)
                    pocket_sdram_line_done <= 1'b1;
                else if (op_is_read)
                    held_valid <= 1'b1;
                state <= S_IDLE;
            end
            S_WAIT: begin
                if (wait_cnt == '0)
                    state <= after;
                else
                    wait_cnt <= wait_cnt - 15'd1;
            end
            default: state <= S_IDLE;
        endcase

        if (!pll_locked) begin
            state <= S_BOOT;
            wait_cnt <= 15'(INIT_WAIT);
            pocket_sdram_ready <= 1'b0;
        end
    end

endmodule
