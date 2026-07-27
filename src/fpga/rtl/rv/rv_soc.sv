/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The soft CPU and its memory: a Hazard3 — the RP2350's own RISC-V core —
 * with a unified 128 KB tightly-coupled RAM and the beginnings of an MMIO
 * bus. This is the processor the RIA firmware runs on; the 6502-facing
 * hardware arrives as MMIO devices.
 *
 * The AHB slave is single-cycle: always ready, reads launched in the address
 * phase out of BRAM, writes landed by byte lane at the end of the data phase.
 *
 * MMIO so far, word-wide at 0xF0000000:
 *   +0  console: write emits the low byte; reads as 0 (always ready)
 *   +4  halt: write stops the simulation testbench, value is the exit code
 */

module rv_soc (
    input logic clk,
    input logic rst_n,

    output logic [7:0] rv_soc_tx_data,
    output logic rv_soc_tx_valid,

    output logic rv_soc_halted,
    output logic [31:0] rv_soc_exit_code
);

    localparam int TCM_WORDS = 32768;  // 128 KB

    // AHB5 master from the CPU. Address bits between the TCM window and the
    // MMIO page have no decode yet, and the bus never bursts.
    logic [31:0] haddr;
    logic hwrite;
    logic [1:0] htrans;
    logic [2:0] hsize;
    logic hready;
    logic [31:0] hwdata, hrdata;
    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_bits;
    always_comb unused_bits = ^{haddr[27:17], htrans[0], hsize[2]};
    /* verilator lint_on UNUSEDSIGNAL */

    /* verilator lint_off PINCONNECTEMPTY */
    hazard3_cpu_1port #(
        .RESET_VECTOR(32'h0000_0000),
        .MTVEC_INIT(32'h0000_0000),
        .NUM_IRQS(1)
    ) cpu (
        .clk(clk),
        .clk_always_on(clk),
        .rst_n(rst_n),
        .pwrup_req(),
        .pwrup_ack(1'b1),
        .clk_en(),
        .unblock_out(),
        .unblock_in(1'b0),
        .haddr(haddr),
        .hwrite(hwrite),
        .htrans(htrans),
        .hsize(hsize),
        .hburst(),
        .hprot(),
        .hmastlock(),
        .hmaster(),
        .hexcl(),
        .hready(hready),
        .hresp(1'b0),
        .hexokay(1'b1),
        .hwdata(hwdata),
        .hrdata(hrdata),
        .fence_i_vld(),
        .fence_d_vld(),
        .fence_rdy(1'b1),
        .dbg_req_halt(1'b0),
        .dbg_req_halt_on_reset(1'b0),
        .dbg_req_resume(1'b0),
        .dbg_halted(),
        .dbg_running(),
        .dbg_data0_rdata(32'h0),
        .dbg_data0_wdata(),
        .dbg_data0_wen(),
        .dbg_instr_data(32'h0),
        .dbg_instr_data_vld(1'b0),
        .dbg_instr_data_rdy(),
        .dbg_instr_caught_exception(),
        .dbg_instr_caught_ebreak(),
        .dbg_sbus_addr(32'h0),
        .dbg_sbus_write(1'b0),
        .dbg_sbus_size(2'h0),
        .dbg_sbus_vld(1'b0),
        .dbg_sbus_rdy(),
        .dbg_sbus_err(),
        .dbg_sbus_wdata(32'h0),
        .dbg_sbus_rdata(),
        .mhartid_val(32'h0),
        .eco_version(4'h0),
        .irq(1'b0),
        .soft_irq(1'b0),
        .timer_irq(1'b0)
    );
    /* verilator lint_on PINCONNECTEMPTY */

    always_comb hready = 1'b1;

    // Address-phase capture; the data phase completes one cycle later.
    logic dph_active, dph_write, dph_mmio;
    logic [14:0] dph_word;  // TCM word index; strb carries the byte lanes
    logic [3:0] dph_strb;
    logic [3:0] mmio_reg;

    logic [3:0] strb;
    always_comb begin
        case (hsize[1:0])
            2'b00: strb = 4'b0001 << haddr[1:0];
            2'b01: strb = haddr[1] ? 4'b1100 : 4'b0011;
            default: strb = 4'b1111;
        endcase
    end

    logic [31:0] tcm[TCM_WORDS] /*verilator public_flat_rw*/;

    logic [31:0] tcm_rdata;
    logic [14:0] word_addr;
    always_comb word_addr = haddr[16:2];

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            dph_active <= 1'b0;
            dph_write <= 1'b0;
            dph_mmio <= 1'b0;
            dph_word <= '0;
            dph_strb <= '0;
            mmio_reg <= '0;
        end else begin
            dph_active <= htrans[1];
            dph_write <= hwrite;
            dph_mmio <= haddr[31:28] == 4'hF;
            dph_word <= haddr[16:2];
            dph_strb <= strb;
            mmio_reg <= haddr[3:0];
        end
    end

    // TCM read launches in the address phase; write lands in the data phase.
    always_ff @(posedge clk) begin
        tcm_rdata <= tcm[word_addr];
        if (dph_active && dph_write && !dph_mmio) begin
            if (dph_strb[0])
                tcm[dph_word][7:0] <= hwdata[7:0];
            if (dph_strb[1])
                tcm[dph_word][15:8] <= hwdata[15:8];
            if (dph_strb[2])
                tcm[dph_word][23:16] <= hwdata[23:16];
            if (dph_strb[3])
                tcm[dph_word][31:24] <= hwdata[31:24];
        end
    end

    always_comb hrdata = dph_mmio ? 32'h0 : tcm_rdata;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            rv_soc_tx_data <= 8'h00;
            rv_soc_tx_valid <= 1'b0;
            rv_soc_halted <= 1'b0;
            rv_soc_exit_code <= 32'h0;
        end else begin
            rv_soc_tx_valid <= 1'b0;
            if (dph_active && dph_write && dph_mmio) begin
                case (mmio_reg)
                    4'h0: begin
                        rv_soc_tx_data <= hwdata[7:0];
                        rv_soc_tx_valid <= 1'b1;
                    end
                    4'h4: begin
                        rv_soc_halted <= 1'b1;
                        rv_soc_exit_code <= hwdata;
                    end
                    default: ;
                endcase
            end
        end
    end

endmodule
