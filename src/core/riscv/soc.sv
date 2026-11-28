/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

module soc
    import mem_pkg::*;
#(
    /* clk's rate, a multiple of 100 kHz. */
    parameter int CLK_KHZ = 50400
) (
    input logic clk,
    input logic rst_n,

    /* The host's ROM landing: rom_set loads rom_len, the length of the
     * image it has staged; rom_upd counts its loads; rom_stop is high
     * while a load is in progress. */
    input logic rom_set,
    input logic [31:0] rom_len,
    input logic [7:0] rom_upd,
    input logic rom_stop,

    output logic [7:0] soc_tx_data,
    output logic soc_tx_valid,

    output logic [15:0] soc_phi2_khz,

    /* mtime stands still while this is high: the platform holds the
     * machine whose clock mtime is, and that machine loses no time. */
    input logic mtime_hold,

    /* The memory is lines of four words behind this port, which the
     * platform answers. cache.sv says how it is used. */
    output logic soc_line_pend,
    output logic soc_line_we,
    output logic [MEM_AW-3:0] soc_line_addr,
    output logic [127:0] soc_line_wdata,
    input logic line_done,
    input logic [127:0] line_rdata,

    output logic soc_halted,
    output logic [31:0] soc_exit_code,

    /* bus_taken is high for one clock, the clock after the platform
     * took the access. Nothing else about the access comes back: the
     * data phase ends a clock after bus_taken, and by then the target
     * has the word. */
    input logic bus_taken,
    output logic soc_bus_pend,
    output logic soc_bus_we,
    output logic [31:0] soc_bus_addr,
    output logic [31:0] soc_bus_wdata,
    output logic [3:0] soc_bus_wstrb,
    input logic [31:0] bus_rdata
);


    logic [31:0] haddr /*verilator public_flat_rd*/;
    logic hwrite /*verilator public_flat_rd*/;
    logic [1:0] htrans /*verilator public_flat_rd*/;
    logic [2:0] hsize /*verilator public_flat_rd*/;
    logic hready /*verilator public_flat_rd*/;
    logic [31:0] hwdata /*verilator public_flat_rd*/;
    logic [31:0] hrdata /*verilator public_flat_rd*/;
    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_bits;
    always_comb unused_bits = ^{haddr[27:MEM_AW+2], htrans[0], hsize[2]};
    /* verilator lint_on UNUSEDSIGNAL */

    /* verilator lint_off PINCONNECTEMPTY */
    hazard3_cpu_1port #(
        .RESET_VECTOR(32'h0000_0000),
        .MTVEC_INIT(32'h0000_0000),
        .NUM_IRQS(1),
        .DEBUG_SUPPORT(0)
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

    logic dph_active /*verilator public_flat_rd*/;
    logic dph_write /*verilator public_flat_rd*/;
    logic dph_mmio /*verilator public_flat_rd*/;
    logic dph_ext /*verilator public_flat_rd*/;
    logic dph_waited /*verilator public_flat_rd*/;
    logic [MEM_AW-1:0] dph_word;
    logic [31:0] dph_addr;
    logic [3:0] dph_strb;
    logic [6:0] mmio_reg;

    logic cache_ready;
    always_comb hready = !(dph_active && dph_ext && !dph_waited)
        && cache_ready;

    logic [3:0] strb;
    always_comb begin
        case (hsize[1:0])
            2'b00: strb = 4'b0001 << haddr[1:0];
            2'b01: strb = haddr[1] ? 4'b1100 : 4'b0011;
            default: strb = 4'b1111;
        endcase
    end

    /* The memory is the top nibble 0. The cache reads its memories at the
     * address of the access after the one in its data phase, in every
     * clock, so it answers a hit with no wait, and a load whose address
     * phase overlaps a store's data phase returns the stored bytes. A
     * miss holds hready low while the line comes through the line port.
     *
     * A store to the ROM block is dropped: the cache sees a load, so the
     * access ends as any other and nothing changes. No line of the ROM
     * block is ever dirty, and the cache never writes one out. */
    logic [31:0] mem_rdata;
    cache #(.MEM_WORDS(MEM_WORDS)) cache (
        .clk(clk),
        .rst_n(rst_n),
        .req_word(haddr[MEM_AW+1:2]),
        .dph(dph_active && !dph_mmio && !dph_ext),
        .dph_write(dph_write && dph_word >= MEM_AW'(ROM_WORDS)),
        .dph_word(dph_word),
        .dph_strb(dph_strb),
        .wdata(hwdata),
        .cache_rdata(mem_rdata),
        .cache_ready(cache_ready),
        .cache_line_pend(soc_line_pend),
        .cache_line_we(soc_line_we),
        .cache_line_addr(soc_line_addr),
        .cache_line_wdata(soc_line_wdata),
        .line_done(line_done),
        .line_rdata(line_rdata)
    );

    initial begin
        dph_active = 1'b0;
        dph_write = 1'b0;
        dph_mmio = 1'b0;
        dph_ext = 1'b0;
        dph_waited = 1'b0;
        dph_word = '0;
        dph_addr = '0;
        dph_strb = '0;
        mmio_reg = '0;
    end
    always_ff @(posedge clk) begin
        if (hready) begin
            dph_active <= htrans[1];
            dph_write <= hwrite;
            dph_mmio <= haddr[31:28] == 4'hF;
            dph_ext <= haddr[31:28] != 4'h0 && haddr[31:28] != 4'hF;
            dph_word <= haddr[MEM_AW+1:2];
            dph_addr <= haddr;
            dph_strb <= strb;
            mmio_reg <= haddr[6:0];
            dph_waited <= 1'b0;
        end else if (bus_taken) begin
            dph_waited <= 1'b1;
        end
    end

    always_comb soc_bus_pend = dph_active && dph_ext && !dph_waited;
    always_comb begin
        soc_bus_we = dph_write;
        soc_bus_addr = dph_addr;
        soc_bus_wdata = hwdata;
        soc_bus_wstrb = dph_strb;
    end

    logic [31:0] mmio_rom_len /*verilator public_flat_rw*/;

    /* An MMIO write, data phase and all, is done on the clock its data
     * phase ends. */
    logic mmio_we;
    always_comb mmio_we = dph_active && dph_write && dph_mmio && hready;

    /* mtime is the machine's clock, which a savestate carries, so it
     * counts only while mtime_hold is low. It is written as two halves,
     * the low one first: a write of the low half is kept until the high
     * half's write puts both in, so a savestate puts the count back
     * whole.
     *
     * A microsecond is CLK_KHZ / 1000 clocks, which need not be whole, so
     * the counters add 10 a clock and step at CLK_KHZ / 100, 504 at
     * 50.4 MHz. */
    localparam logic [15:0] US_ADD = 16'd10;
    localparam logic [15:0] US_LAST = 16'(CLK_KHZ / 100) - US_ADD;
    logic [63:0] mtime_us /*verilator public_flat_rd*/;
    logic [15:0] mtime_acc;
    logic [31:0] mtime_lo;
    initial begin
        mtime_us = 64'd0;
        mtime_acc = '0;
        mtime_lo = '0;
    end
    always_ff @(posedge clk) begin
        if (!mtime_hold) begin
            if (mtime_acc >= US_LAST) begin
                mtime_acc <= mtime_acc - US_LAST;
                mtime_us <= mtime_us + 64'd1;
            end else
                mtime_acc <= mtime_acc + US_ADD;
        end
        if (mmio_we && mmio_reg == 7'h10)
            mtime_lo <= hwdata;
        if (mmio_we && mmio_reg == 7'h14) begin
            mtime_us <= {hwdata, mtime_lo};
            mtime_acc <= '0;
        end
    end

    /* uptime counts microseconds from configuration, with nothing to stop
     * or write it, so the wall clock follows real time through a hold. */
    logic [63:0] uptime_us;
    logic [15:0] uptime_acc;
    initial begin
        uptime_us = 64'd0;
        uptime_acc = '0;
    end
    always_ff @(posedge clk) begin
        if (uptime_acc >= US_LAST) begin
            uptime_acc <= uptime_acc - US_LAST;
            uptime_us <= uptime_us + 64'd1;
        end else
            uptime_acc <= uptime_acc + US_ADD;
    end

    always_comb begin
        if (dph_ext)
            hrdata = bus_rdata;
        else if (dph_mmio)
            case (mmio_reg)
                7'h0C: hrdata = {16'd0, soc_phi2_khz};
                7'h10: hrdata = mtime_us[31:0];
                7'h14: hrdata = mtime_us[63:32];
                7'h18: hrdata = mmio_rom_len;
                7'h1C: hrdata = {31'd0, rom_stop};
                7'h20: hrdata = uptime_us[31:0];
                7'h24: hrdata = uptime_us[63:32];
                7'h4C: hrdata = {24'd0, rom_upd};
                default: hrdata = 32'h0;
            endcase
        else
            hrdata = mem_rdata;
    end

    initial begin
        soc_tx_data = 8'h00;
        soc_tx_valid = 1'b0;
        soc_halted = 1'b0;
        soc_exit_code = 32'h0;
        mmio_rom_len = 32'h0;
        soc_phi2_khz = 16'd8000;
    end
    always_ff @(posedge clk) begin
            soc_tx_valid <= 1'b0;
            if (rom_set)
                mmio_rom_len <= rom_len;
            if (mmio_we) begin
                case (mmio_reg)
                    7'h00: begin
                        soc_tx_data <= hwdata[7:0];
                        soc_tx_valid <= 1'b1;
                    end
                    7'h04: begin
                        soc_halted <= 1'b1;
                        soc_exit_code <= hwdata;
                    end
                    7'h0C: soc_phi2_khz <= hwdata[15:0];
                    7'h18: mmio_rom_len <= hwdata;
                    default: ;
                endcase
            end
    end

endmodule
