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
 *
 * The XSTACK lives here too: 512 bytes plus the guard byte that is the
 * empty stack's top. A write to $FFEC pushes, a read pops, and cell $0C is
 * the top-of-stack mirror both sides maintain — hardware after the 6502's
 * push and pop, the firmware's own API_STACK stores after its. The OS sees
 * the bytes and the pointer through its window, the plain memory that
 * api_push_n and api_pop_n expect.
 *
 * A write to $FFEF is a syscall: the hardware itself arms the BRA -2 block
 * at $FFF1 in the same cycle the op lands, so the 6502 can JSR into the
 * trampoline immediately and there is no window where it outruns the OS
 * (api.h's api_set_regs_blocked becomes a harmless duplicate). The OS sees
 * the op in the cells, does its work, patches the return bytes, and clears
 * the pending flag.
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

    /* The OS side: words 0-7 are the cells, plain memory the way the
     * firmware's REGS macros treat them, wide enough for REGSW and REGSL.
     * Word 16 pops the 6502's TX ring (bit 8 valid), word 18 offers an RX
     * byte and reads back whether the offer slot is free. Words 64-192 are
     * the xstack bytes, word 200 the stack pointer. */
    input logic b_we,
    input logic b_re,
    input logic [7:0] b_word,
    input logic [3:0] b_wstrb,
    input logic [31:0] b_wdata,
    output logic [31:0] ria_regs_b_rdata,

    /* A syscall is pending; the OS acknowledges. */
    output logic ria_regs_api_pending,
    input logic api_ack
);

    localparam logic [7:0] TX_READY = 8'h80;
    localparam logic [7:0] RX_READY = 8'h40;

    logic [7:0] regs[32] /*verilator public_flat_rw*/;

    /* The 6502's TX ring: pushed at $FFE1, drained by the OS. */
    logic [7:0] txf[16];
    logic [3:0] txf_w, txf_r;
    logic [4:0] txf_count;
    logic txf_pop;

    /* The XSTACK: 512 bytes, a zero guard at the top, and the pointer. */
    logic [7:0] xs[516];
    logic [9:0] xsp;
    logic xs_fill;  // a pop's mirror refill lands one clock later
    logic [9:0] xs_fill_at;

    /* The OS's RX offer, merged with the external one; external wins. */
    logic os_rx_valid;
    logic [7:0] os_rx_data;
    logic eff_rx_valid;
    logic [7:0] eff_rx_data;
    always_comb begin
        eff_rx_valid = rx_valid || os_rx_valid;
        eff_rx_data = rx_valid ? rx_data : os_rx_data;
    end

    /* Whether this access pulls the offered byte into the latch. */
    logic pull;
    always_comb begin
        pull = 1'b0;
        if (cs && !we && eff_rx_valid) begin
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
                5'h00: ria_regs_data = (regs[0] & ~TX_READY)
                    | (txf_count < 5'd16 ? TX_READY : 8'h00)
                    | (pull ? RX_READY : 8'h00);
                5'h02: ria_regs_data = regs[2];
                default: ;
            endcase
        end
    end

    /* Registered with the side effect it reports: the offered byte is taken
     * at the same edge that latches it, never a cycle before. */
    logic [9:0] xs_byte;
    always_comb xs_byte = {b_word - 8'd64, 2'b00};

    always_comb begin
        case (b_word)
            8'd16: ria_regs_b_rdata = {23'd0, txf_count != 5'd0, txf[txf_r]};
            8'd18: ria_regs_b_rdata = {31'd0, !os_rx_valid};
            8'd200: ria_regs_b_rdata = {22'd0, xsp};
            default: begin
                if (b_word >= 8'd64 && b_word <= 8'd192)
                    ria_regs_b_rdata = {
                        xs[xs_byte+10'd3], xs[xs_byte+10'd2],
                        xs[xs_byte+10'd1], xs[xs_byte+10'd0]
                    };
                else
                    ria_regs_b_rdata = {
                        regs[{b_word[2:0], 2'd3}], regs[{b_word[2:0], 2'd2}],
                        regs[{b_word[2:0], 2'd1}], regs[{b_word[2:0], 2'd0}]
                    };
            end
        endcase
    end

    /* Popping is the strobed read of word 16 with a byte available. */
    always_comb txf_pop = b_re && b_word == 8'd16 && txf_count != 5'd0;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            for (int i = 0; i < 32; i++)
                regs[i] <= 8'h00;
            ria_regs_tx_data <= 8'h00;
            ria_regs_tx_valid <= 1'b0;
            ria_regs_rx_taken <= 1'b0;
            ria_regs_api_pending <= 1'b0;
        end else if (en) begin
            ria_regs_tx_valid <= 1'b0;
            ria_regs_rx_taken <= pull;
            if (api_ack)
                ria_regs_api_pending <= 1'b0;
            if (cs) begin
                if (we) begin
                    case (rs)
                        5'h01: begin
                            /* Push; a write while full drops, and TX_READY
                             * told the program not to. The tap still pulses
                             * for the testbench console. */
                            ria_regs_tx_data <= data_i;
                            ria_regs_tx_valid <= 1'b1;
                        end
                        5'h0C: begin
                            /* Push: the mirror is the byte just pushed. */
                            if (xsp != 10'd0)
                                regs[5'h0C] <= data_i;
                        end
                        5'h0F: begin
                            /* The op lands and the trampoline blocks, one
                             * indivisible cycle. */
                            regs[5'h0F] <= data_i;
                            regs[5'h11] <= 8'h80;
                            regs[5'h12] <= 8'hFE;
                            ria_regs_api_pending <= 1'b1;
                        end
                        default: regs[rs] <= data_i;
                    endcase
                end else begin
                    case (rs)
                        5'h00: begin
                            if (pull) begin
                                regs[0] <= regs[0] | RX_READY;
                                regs[2] <= eff_rx_data;
                            end
                        end
                        5'h0C: ;  /* pop: pointer and mirror move below */
                        5'h02: begin
                            /* Return the latch, then refill or empty it. */
                            if (pull) begin
                                regs[2] <= eff_rx_data;
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
            if (api_ack)
                ria_regs_api_pending <= 1'b0;
        end
        /* A pop's mirror refill arrives from the RAM one clock after the
         * pointer moved, always between 6502 cycles; an OS write below still
         * outranks it. */
        if (rst_n && xs_fill)
            regs[5'h0C] <= xs[xs_fill_at];
        /* The OS side is plain shared memory at the system clock; it lands
         * regardless of the 6502's enable, and a same-cell collision goes to
         * the OS, as arbitrary as it is on the real dual-core part. */
        if (rst_n && b_we && b_word < 8'd8) begin
            if (b_wstrb[0])
                regs[{b_word[2:0], 2'd0}] <= b_wdata[7:0];
            if (b_wstrb[1])
                regs[{b_word[2:0], 2'd1}] <= b_wdata[15:8];
            if (b_wstrb[2])
                regs[{b_word[2:0], 2'd2}] <= b_wdata[23:16];
            if (b_wstrb[3])
                regs[{b_word[2:0], 2'd3}] <= b_wdata[31:24];
        end
    end

    /* The rings live at the system clock: the 6502 pushes on its enable, the
     * OS pops and offers whenever its strobe lands. */
    logic push_now;
    always_comb push_now = en && cs && we && rs == 5'h01 && txf_count < 5'd16;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            txf_w <= 4'd0;
            txf_r <= 4'd0;
            txf_count <= 5'd0;
            os_rx_valid <= 1'b0;
            os_rx_data <= 8'h00;
            xsp <= 10'd512;
            xs_fill <= 1'b0;
            xs_fill_at <= 10'd0;
        end else begin
            /* The 6502's stack ops, and the pop's one-clock-late refill of
             * the top-of-stack mirror out of the RAM. */
            xs_fill <= 1'b0;
            if (en && cs && rs == 5'h0C) begin
                if (we) begin
                    if (xsp != 10'd0) begin
                        xs[xsp-10'd1] <= data_i;
                        xsp <= xsp - 10'd1;
                    end
                end else begin
                    if (xsp != 10'd512) begin
                        xsp <= xsp + 10'd1;
                        xs_fill <= 1'b1;
                        xs_fill_at <= xsp + 10'd1;
                    end else begin
                        xs_fill <= 1'b1;
                        xs_fill_at <= xsp;
                    end
                end
            end
            /* OS writes: xstack bytes by lane, or the pointer. */
            if (b_we && b_word >= 8'd64 && b_word <= 8'd192) begin
                if (b_wstrb[0])
                    xs[xs_byte+10'd0] <= b_wdata[7:0];
                if (b_wstrb[1])
                    xs[xs_byte+10'd1] <= b_wdata[15:8];
                if (b_wstrb[2])
                    xs[xs_byte+10'd2] <= b_wdata[23:16];
                if (b_wstrb[3])
                    xs[xs_byte+10'd3] <= b_wdata[31:24];
            end
            if (b_we && b_word == 8'd200)
                xsp <= b_wdata[9:0];
            if (push_now) begin
                txf[txf_w] <= data_i;
                txf_w <= txf_w + 4'd1;
            end
            if (txf_pop)
                txf_r <= txf_r + 4'd1;
            txf_count <= txf_count + {4'd0, push_now} - {4'd0, txf_pop};
            if (b_we && b_word == 8'd18 && !os_rx_valid) begin
                os_rx_valid <= 1'b1;
                os_rx_data <= b_wdata[7:0];
            end else if (en && pull && !rx_valid) begin
                os_rx_valid <= 1'b0;
            end
        end
    end

endmodule
