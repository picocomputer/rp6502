/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The staging store: the Pocket's 64 MB SDR SDRAM under a plain
 * correctness-first controller at 100.8 MHz — CL2, single beats with
 * auto-precharge, one op in flight, refresh on a due counter. The
 * loader's halfword writes pull from the bridge and win the bus only
 * when no read is pending; the staging read serves-and-holds, so the
 * machine's stalled byte fetch completes the moment its halfword
 * stands, and repeat fetches inside the held halfword cost nothing.
 * A write to the held address drops the hold. Bandwidth is beneath
 * consideration here — a ROM loads in tens of milliseconds and the
 * machine reads bytes — so every timing wait is padded generously.
 */

module pocket_sdram (
    input logic clk,
    input logic rst_n,

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

    /* 200 us at 100.8 MHz, then the JEDEC wake: precharge all, two
     * refreshes, mode register CL2 burst-1. */
    localparam int INIT_WAIT = 20200;
    localparam logic [12:0] MODE_CL2_BL1 = 13'b000_0_00_010_0_000;
    localparam int REFRESH_EVERY = 780; /* 7.7 us, 8192 rows in 64 ms */

    typedef enum logic [3:0] {
        S_BOOT, S_PALL, S_REF0, S_REF1, S_MRS,
        S_IDLE, S_REFRESH, S_ACT, S_READ, S_WRITE, S_WAIT
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

    always_comb begin
        dram_cke = 1'b1;
        dram_dqm = 2'b00;
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= S_BOOT;
            after <= S_IDLE;
            wait_cnt <= 15'(INIT_WAIT);
            ref_cnt <= '0;
            ref_due <= '0;
            op_is_read <= 1'b0;
            op_addr <= '0;
            op_wdata <= '0;
            held_addr <= '0;
            held_valid <= 1'b0;
            rd_pipe <= '0;
            pocket_sdram_rdata <= '0;
            pocket_sdram_wtake <= 1'b0;
            pocket_sdram_ready <= 1'b0;
            dram_a <= '0;
            dram_ba <= '0;
            dram_ras_n <= 1'b1;
            dram_cas_n <= 1'b1;
            dram_we_n <= 1'b1;
            dram_dq_out <= '0;
            dram_dq_oe <= 1'b0;
        end else begin
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
                S_IDLE: begin
                    if (ref_due != '0) begin
                        state <= S_REFRESH;
                    end else if (rd_pend && !pocket_sdram_rvalid) begin
                        op_is_read <= 1'b1;
                        op_addr <= rd_addr;
                        state <= S_ACT;
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
                        state <= S_ACT;
                    end
                end
                S_REFRESH: begin
                    {dram_ras_n, dram_cas_n, dram_we_n} <= 3'b001;
                    ref_due <= ref_due - 2'd1;
                    wait_cnt <= 15'd8;
                    after <= S_IDLE;
                    state <= S_WAIT;
                end
                S_ACT: begin
                    {dram_ras_n, dram_cas_n, dram_we_n} <= 3'b011;
                    dram_ba <= op_addr[24:23];
                    dram_a <= op_addr[22:10];
                    wait_cnt <= 15'd1; /* tRCD: command lands next clock */
                    after <= op_is_read ? S_READ : S_WRITE;
                    state <= S_WAIT;
                end
                S_READ: begin
                    {dram_ras_n, dram_cas_n, dram_we_n} <= 3'b101;
                    dram_ba <= op_addr[24:23];
                    dram_a <= {3'b001, op_addr[9:0]}; /* A10 autopre */
                    rd_pipe[0] <= 1'b1;
                    wait_cnt <= 15'd5;
                    after <= S_IDLE;
                    state <= S_WAIT;
                end
                S_WRITE: begin
                    {dram_ras_n, dram_cas_n, dram_we_n} <= 3'b100;
                    dram_ba <= op_addr[24:23];
                    dram_a <= {3'b001, op_addr[9:0]}; /* A10 autopre */
                    dram_dq_out <= op_wdata;
                    dram_dq_oe <= 1'b1;
                    wait_cnt <= 15'd5;
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
    end

endmodule
