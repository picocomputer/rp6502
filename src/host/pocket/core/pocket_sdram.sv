/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

module pocket_sdram #(
    parameter bit SELF_REFRESH = 1'b1,
    parameter bit CLOSE_IDLE_ROWS = 1'b1
) (
    input logic clk,

    input logic rd_pend,
    input logic [24:0] rd_addr,
    output logic [15:0] pocket_sdram_rdata,
    output logic pocket_sdram_rvalid,

    input logic w_avail,
    input logic [24:0] w_addr,
    input logic [15:0] w_data,
    output logic pocket_sdram_wtake,

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
        S_IDLE, S_REFRESH, S_PALL_R, S_PALL_I, S_PRE, S_ACT, S_READ,
        S_WRITE, S_SREF_ENT, S_SREF, S_WAIT
    } state_t;
    state_t state, after;

    logic [14:0] wait_cnt;
    logic [9:0] ref_cnt;
    logic [1:0] ref_due;
    logic [9:0] idle_cnt;
    logic [3:0] sref_cnt;

    logic op_is_read;
    logic [24:0] op_addr;
    logic [15:0] op_wdata;

    logic [24:0] held_addr;
    logic held_valid;
    always_comb pocket_sdram_rvalid = held_valid && held_addr == rd_addr;

    logic [3:0] rd_pipe;

    logic [12:0] row_open[4];
    logic bank_active[4];
    logic any_active;
    always_comb any_active = bank_active[0] || bank_active[1]
                          || bank_active[2] || bank_active[3];

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

    always_comb dram_dqm = 2'b00;

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

        if (pocket_sdram_ready && state != S_SREF) begin
            if (ref_cnt == 10'(REFRESH_EVERY - 1)) begin
                ref_cnt <= '0;
                if (ref_due != 2'd3)
                    ref_due <= ref_due + 2'd1;
            end else begin
                ref_cnt <= ref_cnt + 10'd1;
            end
        end

        rd_pipe <= {rd_pipe[2:0], 1'b0};
        if (rd_pipe[3]) begin
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
                if (ref_due != '0) begin
                    idle_cnt <= '0;
                    state <= any_active ? S_PALL_R : S_REFRESH;
                end else if (rd_pend && !pocket_sdram_rvalid) begin
                    idle_cnt <= '0;
                    op_is_read <= 1'b1;
                    op_addr <= rd_addr;
                    state <= req_hit ? S_READ
                           : (req_needs_pre ? S_PRE : S_ACT);
                end else if (w_avail) begin
                    idle_cnt <= '0;
                    op_is_read <= 1'b0;
                    op_addr <= w_addr;
                    op_wdata <= w_data;
                    pocket_sdram_wtake <= 1'b1;
                    /* held_valid is cleared when the write is accepted
                     * rather than when its WRITE command is issued, so
                     * a read of this address in between waits for the
                     * new data instead of returning the old. */
                    if (held_valid && held_addr == w_addr)
                        held_valid <= 1'b0;
                    state <= req_hit ? S_WRITE
                           : (req_needs_pre ? S_PRE : S_ACT);
                end else begin
                    if (!rd_pend && !w_avail)
                        idle_cnt <= idle_cnt + 10'd1;
                    if (CLOSE_IDLE_ROWS && idle_cnt == 10'(IDLE_CLOSE)
                        && any_active)
                        state <= S_PALL_I;
                    else if (SELF_REFRESH && idle_cnt == 10'(IDLE_SREF)
                             && !any_active)
                        state <= S_SREF_ENT;
                end
            end
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
                else if (rd_pend || w_avail) begin
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
             * ACTIVE is followed by its READ or WRITE before S_IDLE runs
             * again, which puts at least eight clocks between an ACTIVE
             * and any precharge. */
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
             * the row stays open after the access. */
            S_READ: begin
                {dram_ras_n, dram_cas_n, dram_we_n} <= 3'b101;
                dram_ba <= op_addr[24:23];
                dram_a <= {3'b000, op_addr[9:0]};
                rd_pipe[0] <= 1'b1;
                wait_cnt <= 15'd3;
                after <= S_IDLE;
                state <= S_WAIT;
            end
            S_WRITE: begin
                {dram_ras_n, dram_cas_n, dram_we_n} <= 3'b100;
                dram_ba <= op_addr[24:23];
                dram_a <= {3'b000, op_addr[9:0]};
                dram_dq_out <= op_wdata;
                dram_dq_oe <= 1'b1;
                wait_cnt <= 15'd2; /* tWR */
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
