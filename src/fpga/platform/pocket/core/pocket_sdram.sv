/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The staging store: the Pocket's 64 MB SDR SDRAM under a plain
 * correctness-first controller at 50.4 MHz — CL2, single beats, one op
 * in flight, refresh on a due counter. The loader's halfword writes
 * pull from the bridge and win the bus only when no read is pending;
 * the staging read serves-and-holds, so the machine's stalled byte
 * fetch completes the moment its halfword stands, and repeat fetches
 * inside the held halfword cost nothing. A write to the held address
 * drops the hold.
 *
 * Rows are left open. Every access used to set A10 and throw its row
 * away, which made an activate the price of every byte: eight clocks
 * to data and eleven before the next op could start. That was fine
 * while nothing here was in a hurry — the header used to say bandwidth
 * was beneath consideration — and it stops being fine the moment the
 * 6502's RAM lives here. At 8 MHz phi2_div emits pulses six clocks
 * apart at their tightest, and an activate does not fit in six clocks
 * while a column access does.
 *
 * So each bank remembers what it is holding, a request that matches
 * goes straight to the column access, and only a genuine row change
 * pays the precharge. Refresh is the one miss nothing can avoid: it
 * requires every bank precharged, so it closes all four.
 */

module pocket_sdram (
    input logic clk,

    /* Staging reads: pend-and-hold, halfword addressing. */
    input logic rd_pend,
    input logic [24:0] rd_addr,
    output logic [15:0] pocket_sdram_rdata,
    output logic pocket_sdram_rvalid,

    /* Loader writes: pulled when available, lowest priority. */
    input logic w_avail,
    input logic [24:0] w_addr,
    input logic [15:0] w_data,
    output logic pocket_sdram_wtake,

    output logic pocket_sdram_ready,

    /* The chip, dq split for the pad ring. */
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

    /* 200 us at 50.4 MHz, then the JEDEC wake: precharge all, two
     * refreshes, mode register CL2 burst-1. */
    localparam int INIT_WAIT = 10100;
    localparam logic [12:0] MODE_CL2_BL1 = 13'b000_0_00_010_0_000;
    localparam int REFRESH_EVERY = 390; /* 7.7 us, 8192 rows in 64 ms */

    typedef enum logic [3:0] {
        S_BOOT, S_PALL, S_REF0, S_REF1, S_MRS,
        S_IDLE, S_REFRESH, S_PALL_R, S_PRE, S_ACT, S_READ, S_WRITE, S_WAIT
    } state_t;
    state_t state, after;

    logic [14:0] wait_cnt;
    logic [9:0] ref_cnt;
    logic [1:0] ref_due;

    logic op_is_read;
    logic [24:0] op_addr;
    logic [15:0] op_wdata;

    /* The held answer. */
    logic [24:0] held_addr;
    logic held_valid;
    always_comb pocket_sdram_rvalid = held_valid && held_addr == rd_addr;

    logic [2:0] rd_pipe;

    /* What each bank is holding open. Every access used to set A10 and
     * throw the row away, which made the activate the price of every
     * single byte. The 6502 is about to run out of here at 8 MHz, where
     * the whole budget is six clocks: an activate does not fit in that
     * and a column access does. */
    logic [12:0] row_open[4];
    logic bank_active[4];
    logic any_active;
    always_comb any_active = bank_active[0] || bank_active[1]
                          || bank_active[2] || bank_active[3];

    /* The request under consideration, decoded before it is latched so
     * S_IDLE can jump straight to the column access on a hit. */
    logic [1:0] req_bank;
    logic [12:0] req_row;
    logic req_hit, req_needs_pre;
    always_comb begin
        if (rd_pend && !pocket_sdram_rvalid) begin
            req_bank = rd_addr[24:23];
            req_row  = rd_addr[22:10];
        end else begin
            req_bank = w_addr[24:23];
            req_row  = w_addr[22:10];
        end
        req_hit = bank_active[req_bank] && row_open[req_bank] == req_row;
        req_needs_pre = bank_active[req_bank] && !req_hit;
    end

    always_comb begin
        dram_cke = 1'b1;
        dram_dqm = 2'b00;
    end

    initial begin
        state = S_BOOT;
        after = S_IDLE;
        wait_cnt = 15'(INIT_WAIT);
        ref_cnt = '0;
        ref_due = '0;
        op_is_read = 1'b0;
        op_addr = '0;
        op_wdata = '0;
        held_addr = '0;
        held_valid = 1'b0;
        rd_pipe = '0;
        for (int b = 0; b < 4; b++) begin
            row_open[b] = '0;
            bank_active[b] = 1'b0;
        end
        pocket_sdram_rdata = '0;
        pocket_sdram_wtake = 1'b0;
        pocket_sdram_ready = 1'b0;
        dram_a = '0;
        dram_ba = '0;
        dram_ras_n = 1'b1;
        dram_cas_n = 1'b1;
        dram_we_n = 1'b1;
        dram_dq_out = '0;
        dram_dq_oe = 1'b0;
    end
    always_ff @(posedge clk) begin
        {dram_ras_n, dram_cas_n, dram_we_n} <= 3'b111;
        dram_dq_oe <= 1'b0;
        pocket_sdram_wtake <= 1'b0;

        if (pocket_sdram_ready) begin
            if (ref_cnt == 10'(REFRESH_EVERY - 1)) begin
                ref_cnt <= '0;
                if (ref_due != 2'd3)
                    ref_due <= ref_due + 2'd1;
            end else begin
                ref_cnt <= ref_cnt + 10'd1;
            end
        end

        /* The registered command plus CL2: data stands three
         * edges after the read state. */
        rd_pipe <= {rd_pipe[1:0], 1'b0};
        if (rd_pipe[2]) begin
            pocket_sdram_rdata <= dram_dq_in;
            held_addr <= op_addr;
            held_valid <= 1'b1;
        end

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
                wait_cnt <= 15'd2;
                after <= S_IDLE;
                state <= S_WAIT;
                pocket_sdram_ready <= 1'b1;
            end
            /* A hit goes straight to the column access; op_addr is
             * registered here and stands by the time that state runs. */
            S_IDLE: begin
                if (ref_due != '0) begin
                    /* Auto-refresh wants every bank precharged. */
                    state <= any_active ? S_PALL_R : S_REFRESH;
                end else if (rd_pend && !pocket_sdram_rvalid) begin
                    op_is_read <= 1'b1;
                    op_addr <= rd_addr;
                    state <= req_hit ? S_READ
                           : (req_needs_pre ? S_PRE : S_ACT);
                end else if (w_avail) begin
                    op_is_read <= 1'b0;
                    op_addr <= w_addr;
                    op_wdata <= w_data;
                    pocket_sdram_wtake <= 1'b1;
                    /* The hold dies at accept, not at the command
                     * — a reader between the two must wait for the
                     * new data, not be served the old. */
                    if (held_valid && held_addr == w_addr)
                        held_valid <= 1'b0;
                    state <= req_hit ? S_WRITE
                           : (req_needs_pre ? S_PRE : S_ACT);
                end
            end
            /* Refresh cannot run with a row open anywhere, so the rows
             * do not survive it. That is the one unavoidable miss. */
            S_PALL_R: begin
                {dram_ras_n, dram_cas_n, dram_we_n} <= 3'b010;
                dram_a <= 13'h400; /* A10: all banks */
                for (int b = 0; b < 4; b++)
                    bank_active[b] <= 1'b0;
                wait_cnt <= 15'd1; /* tRP */
                after <= S_REFRESH;
                state <= S_WAIT;
            end
            S_REFRESH: begin
                {dram_ras_n, dram_cas_n, dram_we_n} <= 3'b001;
                ref_due <= ref_due - 2'd1;
                wait_cnt <= 15'd8;
                after <= S_IDLE;
                state <= S_WAIT;
            end
            /* Only reached when this bank is holding the wrong row.
             * tRAS is met by construction: the activate that opened it
             * is at least a whole operation behind. */
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
                wait_cnt <= 15'd1; /* tRCD: command lands next clock */
                after <= op_is_read ? S_READ : S_WRITE;
                state <= S_WAIT;
            end
            /* A10 stays low: the row is left standing for whoever asks
             * next. The wait is what it takes for rd_pipe to land the
             * data before another accept can move op_addr under it. */
            S_READ: begin
                {dram_ras_n, dram_cas_n, dram_we_n} <= 3'b101;
                dram_ba <= op_addr[24:23];
                dram_a <= {3'b000, op_addr[9:0]};
                rd_pipe[0] <= 1'b1;
                wait_cnt <= 15'd2;
                after <= S_IDLE;
                state <= S_WAIT;
            end
            S_WRITE: begin
                {dram_ras_n, dram_cas_n, dram_we_n} <= 3'b100;
                dram_ba <= op_addr[24:23];
                dram_a <= {3'b000, op_addr[9:0]};
                dram_dq_out <= op_wdata;
                dram_dq_oe <= 1'b1;
                wait_cnt <= 15'd2; /* tWR before anything precharges */
                after <= S_IDLE;
                state <= S_WAIT;
            end
            S_WAIT: begin
                if (wait_cnt == '0)
                    state <= after;
                else
                    wait_cnt <= wait_cnt - 15'd1;
            end
            default: state <= S_IDLE;
        endcase
    end

endmodule
