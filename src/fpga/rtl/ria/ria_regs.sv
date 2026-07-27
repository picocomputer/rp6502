/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The RIA register window at $FFE0-$FFFF, or as much of it as exists before
 * the soft CPU arrives: the bare UART at $FFE0-$FFE2 and the plain RAM cells
 * everything else falls back to, including the 6502 vectors the loader
 * writes. Semantics per emu/sys/ria.c: reading $FFE0 pulls a byte into the
 * $FFE2 latch and reports it in bit 6, bit 7 says TX is always ready;
 * reading $FFE2 returns the latch and refills it.
 *
 * Read data is combinational from pre-tick state; side effects land at the
 * enabled edge, the same discipline as via.sv.
 */

module ria_regs (
    input logic clk,
    input logic rst_n,
    input logic en,

    input logic cs,
    input logic we,
    input logic [4:0] rs,
    input logic [7:0] data_i,

    output logic [7:0] ria_regs_data,

    /* TX: one byte per $FFE1 write, for the console. */
    output logic [7:0] ria_regs_tx_data,
    output logic ria_regs_tx_valid,

    /* RX: a byte offered from the console side, taken when latched. */
    input logic rx_valid,
    input logic [7:0] rx_data,
    output logic ria_regs_rx_taken,

    /* The OS side: the soft CPU reads and writes the cells directly, the
     * way the firmware's REGS() macros treat them as memory. Plain array
     * access at the system clock, no side effects. */
    input logic b_we,
    input logic [4:0] b_rs,
    input logic [7:0] b_wdata,
    output logic [7:0] ria_regs_b_rdata
);

    localparam logic [7:0] TX_READY = 8'h80;
    localparam logic [7:0] RX_READY = 8'h40;

    logic [7:0] regs[32] /*verilator public_flat_rw*/;

    /* Whether this access pulls the offered byte into the latch. */
    logic pull;
    always_comb begin
        pull = 1'b0;
        if (cs && !we && rx_valid) begin
            if (rs == 5'h00 && (regs[0] & RX_READY) == 8'h00)
                pull = 1'b1;
            if (rs == 5'h02)
                pull = 1'b1;
        end
    end

    always_comb begin
        ria_regs_data = regs[rs];
        if (cs && !we) begin
            case (rs)
                5'h00: ria_regs_data = regs[0] | TX_READY
                    | (pull ? RX_READY : 8'h00);
                5'h02: ria_regs_data = regs[2];
                default: ;
            endcase
        end
    end

    /* Registered with the side effect it reports: the offered byte is taken
     * at the same edge that latches it, never a cycle before. */
    always_comb ria_regs_b_rdata = regs[b_rs];

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            for (int i = 0; i < 32; i++)
                regs[i] <= 8'h00;
            ria_regs_tx_data <= 8'h00;
            ria_regs_tx_valid <= 1'b0;
            ria_regs_rx_taken <= 1'b0;
        end else if (en) begin
            ria_regs_tx_valid <= 1'b0;
            ria_regs_rx_taken <= pull;
            if (cs) begin
                if (we) begin
                    case (rs)
                        5'h01: begin
                            ria_regs_tx_data <= data_i;
                            ria_regs_tx_valid <= 1'b1;
                            regs[0] <= regs[0] | TX_READY;
                        end
                        default: regs[rs] <= data_i;
                    endcase
                end else begin
                    case (rs)
                        5'h00: begin
                            regs[0] <= regs[0] | TX_READY
                                | (pull ? RX_READY : 8'h00);
                            if (pull)
                                regs[2] <= rx_data;
                        end
                        5'h02: begin
                            /* Return the latch, then refill or empty it. */
                            if (pull) begin
                                regs[2] <= rx_data;
                                regs[0] <= regs[0] | RX_READY;
                            end else begin
                                regs[2] <= 8'h00;
                                regs[0] <= regs[0] & ~RX_READY;
                            end
                        end
                        default: ;
                    endcase
                end
            end
        end else begin
            ria_regs_tx_valid <= 1'b0;
            ria_regs_rx_taken <= 1'b0;
        end
        /* The OS side is plain shared memory at the system clock; it lands
         * regardless of the 6502's enable, and a same-cell collision goes to
         * the OS, as arbitrary as it is on the real dual-core part. */
        if (rst_n && b_we)
            regs[b_rs] <= b_wdata;
    end

endmodule
