/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

module soc
    import mem_pkg::*;
#(
    parameter int MTIME_ADD = 1,
    parameter int MTIME_WRAP = 1
) (
    input logic clk,
    input logic rst_n,

    input logic slot_set,
    input logic [31:0] slot_len,
    input logic [7:0] upd_n,
    input logic key_set,
    input logic [8:0] key_code,
    input logic [3:0][31:0] cont_key,
    input logic [3:0][31:0] cont_joy,
    input logic [3:0][15:0] cont_trig,
    output logic soc_key_pending,

    output logic [7:0] soc_tx_data,
    output logic soc_tx_valid,

    output logic [15:0] soc_phi2_khz,

    input logic sst_dbg_halt,
    input logic sst_dbg_halt_on_reset,
    input logic sst_dbg_resume,
    output logic soc_dbg_halted,
    input logic [31:0] sst_dbg_data0,
    output logic [31:0] soc_dbg_data0,
    output logic soc_dbg_data0_wen,
    input logic [31:0] sst_dbg_instr,
    input logic sst_dbg_instr_vld,
    output logic soc_dbg_instr_rdy,
    output logic soc_dbg_ebreak,
    output logic soc_dbg_fault,

    input logic sst_phi2_we,
    input logic [15:0] sst_phi2_wdata,
    output logic [63:0] soc_mtime,
    input logic sst_mtime_we,
    input logic [63:0] sst_mtime_wdata,

    /* The memory is lines of four words behind this port, which the
     * platform answers. cache.sv says how it is used. sst_flush and
     * sst_empty are raised only while the core is halted, when it makes
     * no access. */
    input logic sst_flush,
    input logic sst_empty,
    output logic soc_cache_idle,
    output logic soc_line_pend,
    output logic soc_line_we,
    output logic [MEM_AW-3:0] soc_line_addr,
    output logic [127:0] soc_line_wdata,
    input logic line_done,
    input logic [127:0] line_rdata,

    output logic soc_halted,
    output logic [31:0] soc_exit_code,

    /* wiring sets bus_taken on its own clock, on the first edge that
     * finds the access pending and its target able to finish it, and
     * holds it until soc_bus_pend drops. Nothing else about the access
     * comes back: the data phase ends one clk cycle after the request is
     * withdrawn, and by then the target has the word. */
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
    /* The core makes its data0 write from a register through its CSR
     * path, and the falling edge that takes it in wiring is half a
     * system clock on, so it is registered here first. */
    logic [31:0] dbg_data0_w;
    logic dbg_data0_wen_w;
    initial begin
        soc_dbg_data0 = '0;
        soc_dbg_data0_wen = 1'b0;
    end
    always_ff @(posedge clk) begin
        soc_dbg_data0 <= dbg_data0_w;
        soc_dbg_data0_wen <= dbg_data0_wen_w;
    end

    /* The core takes an instruction at its debug port only while its
     * fetch path is at rest. It jumps when it enters debug mode and at
     * every ebreak, and a jump drops an instruction given on that clock
     * and sends a fetch out. While that fetch waits on the cache, the
     * next jump would also drop the core's ready, which reads as an
     * instruction taken. So the port is ready, and an instruction valid,
     * only with no jump and no bus access in progress. */
    logic dbg_rest, dbg_instr_rdy_w;

    hazard3_cpu_1port #(
        .RESET_VECTOR(32'h0000_0000),
        .MTVEC_INIT(32'h0000_0000),
        .NUM_IRQS(1),
        .DEBUG_SUPPORT(1)
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
        .dbg_req_halt(sst_dbg_halt),
        .dbg_req_halt_on_reset(sst_dbg_halt_on_reset),
        .dbg_req_resume(sst_dbg_resume),
        .dbg_halted(soc_dbg_halted),
        .dbg_running(),
        .dbg_data0_rdata(sst_dbg_data0),
        .dbg_data0_wdata(dbg_data0_w),
        .dbg_data0_wen(dbg_data0_wen_w),
        .dbg_instr_data(sst_dbg_instr),
        .dbg_instr_data_vld(sst_dbg_instr_vld && dbg_rest),
        .dbg_instr_data_rdy(dbg_instr_rdy_w),
        .dbg_instr_caught_exception(soc_dbg_fault),
        .dbg_instr_caught_ebreak(soc_dbg_ebreak),
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

    always_comb begin
        dbg_rest = !dph_active && !soc_dbg_ebreak && !soc_dbg_fault;
        soc_dbg_instr_rdy = dbg_instr_rdy_w && dbg_rest;
    end

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
        .flush(sst_flush),
        .empty(sst_empty),
        .cache_idle(soc_cache_idle),
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
        /* soc_bus_we holds for the whole request, because when wiring is
         * built with EXT_RAM set, pocket_sram captures the write enable
         * when it starts a port B access, which is before the access is
         * taken. */
        soc_bus_we = dph_write;
        soc_bus_addr = dph_addr;
        soc_bus_wdata = hwdata;
        soc_bus_wstrb = dph_strb;
    end

    logic [7:0] mmio_kbd_data /*verilator public_flat_rw*/;
    logic mmio_kbd_valid /*verilator public_flat_rw*/;
    logic [8:0] mmio_key_data /*verilator public_flat_rw*/;
    logic mmio_key_valid /*verilator public_flat_rw*/;
    always_comb soc_key_pending = mmio_key_valid;
    logic [31:0] mmio_slot_len /*verilator public_flat_rw*/;

    logic [63:0] mtime_us /*verilator public_flat_rd*/;
    logic [15:0] mtime_acc;
    initial begin
        mtime_us = 64'd0;
        mtime_acc = '0;
    end
    always_comb soc_mtime = mtime_us;
    /* mtime_us does not count while the core is halted, because Hazard3
     * hardwires dcsr.stoptime to 1, which means that timers local to the
     * core do not count in debug mode. */
    always_ff @(posedge clk) begin
        if (!soc_dbg_halted) begin
            if ({16'd0, mtime_acc} + 32'(MTIME_ADD) >= 32'(MTIME_WRAP))
            begin
                mtime_acc <= 16'(32'(mtime_acc) + 32'(MTIME_ADD)
                                 - 32'(MTIME_WRAP));
                mtime_us <= mtime_us + 64'd1;
            end else begin
                mtime_acc <= mtime_acc + 16'(MTIME_ADD);
            end
        end
        if (sst_mtime_we) begin
            mtime_us <= sst_mtime_wdata;
            mtime_acc <= '0;
        end
    end

    always_comb begin
        if (dph_ext)
            hrdata = bus_rdata;
        else if (dph_mmio)
            case (mmio_reg)
                7'h08: hrdata = {23'd0, mmio_kbd_valid, mmio_kbd_data};
                7'h0C: hrdata = {16'd0, soc_phi2_khz};
                7'h10: hrdata = mtime_us[31:0];
                7'h14: hrdata = mtime_us[63:32];
                7'h18: hrdata = mmio_slot_len;
                7'h4C: hrdata = {24'd0, upd_n};
                7'h1C: hrdata = {22'd0, mmio_key_valid, mmio_key_data};
                7'h50: hrdata = cont_key[0];
                7'h54: hrdata = cont_joy[0];
                7'h58: hrdata = {16'd0, cont_trig[0]};
                7'h5C: hrdata = cont_key[1];
                7'h60: hrdata = cont_joy[1];
                7'h64: hrdata = {16'd0, cont_trig[1]};
                7'h68: hrdata = cont_key[2];
                7'h6C: hrdata = cont_joy[2];
                7'h70: hrdata = {16'd0, cont_trig[2]};
                7'h74: hrdata = cont_key[3];
                7'h78: hrdata = cont_joy[3];
                7'h7C: hrdata = {16'd0, cont_trig[3]};
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
        mmio_kbd_valid = 1'b0;
        mmio_kbd_data = 8'h00;
        mmio_key_valid = 1'b0;
        mmio_key_data = 9'h000;
        mmio_slot_len = 32'h0;
        soc_phi2_khz = 16'd8000;
    end
    always_ff @(posedge clk) begin
            soc_tx_valid <= 1'b0;
            if (dph_active && !dph_write && dph_mmio && mmio_reg == 7'h08)
                mmio_kbd_valid <= 1'b0;
            if (dph_active && !dph_write && dph_mmio && mmio_reg == 7'h1C)
                mmio_key_valid <= 1'b0;
            if (slot_set)
                mmio_slot_len <= slot_len;
            if (key_set) begin
                mmio_key_data <= key_code;
                mmio_key_valid <= 1'b1;
            end
            if (dph_active && dph_write && dph_mmio) begin
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
                    7'h18: mmio_slot_len <= hwdata;
                    default: ;
                endcase
            end
            /* sst_phi2_we is tested after the register writes so that
             * sst_phi2_wdata takes priority over a core write to
             * soc_phi2_khz on the same edge. */
            if (sst_phi2_we) soc_phi2_khz <= sst_phi2_wdata;
    end

endmodule
