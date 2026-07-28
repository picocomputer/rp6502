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
 *   +8  keyboard in: one offered byte, bit 8 valid, popped by the read;
 *       the testbench fills it now, the APF input bridge will later
 *   +16 microseconds since reset, low word; +20 high word. The tick is
 *       a fractional accumulator, MTIME_ADD per clock wrapping at
 *       MTIME_WRAP (1/1 in simulation; 10/1008 at the Pocket's
 *       100.8 MHz); the firmware's time_us_64 reads hi-lo-hi
 *   +24 staged ROM length in bytes: the platform sets it after filling
 *       the staging window, the firmware writes 0 once consumed
 *   +28 HID key event: bit 9 valid, bit 8 down, low byte the HID
 *       keycode; popped by the read; the testbench fills it now, the
 *       APF input bridge will later
 *
 * Everything outside the TCM and the local page goes out the system bus,
 * with one wait state so the machine's single-cycle devices — SRAM port B,
 * the RIA cells, the 6502's reset — see one strobe per access and answer by
 * the stretched data phase. Byte lanes carry sb/lb; the loader moves bytes,
 * which is all a 6502's memory wants.
 */

module rv_soc #(
    parameter int MTIME_ADD = 1,
    parameter int MTIME_WRAP = 1,
    parameter TCM_INIT_FILE = ""
) (
    input logic clk,
    input logic rst_n,

    /* Platform sidebands: the APF bridge posts the slot length and key
     * events the way the testbenches poke them. */
    input logic slot_set,
    input logic [31:0] slot_len,
    input logic key_set,
    input logic [8:0] key_code,
    output logic rv_soc_key_pending,

    output logic [7:0] rv_soc_tx_data,
    output logic rv_soc_tx_valid,

    output logic rv_soc_halted,
    output logic [31:0] rv_soc_exit_code,

    /* System bus: one strobe per access during the stretched data phase;
     * bus_rdy low withholds the strobe and stretches the phase, for slaves
     * whose port is arbitrated. */
    input logic bus_rdy,
    output logic rv_soc_bus_pend,
    output logic rv_soc_bus_stb,
    output logic rv_soc_bus_we,
    output logic [31:0] rv_soc_bus_addr,
    output logic [31:0] rv_soc_bus_wdata,
    output logic [3:0] rv_soc_bus_wstrb,
    input logic [31:0] bus_rdata
);

    localparam int TCM_WORDS = 32768;  // 128 KB

    // AHB5 master from the CPU. Address bits between the TCM window and the
    // MMIO page have no decode yet, and the bus never bursts.
    logic [31:0] haddr /*verilator public_flat_rd*/;
    logic hwrite /*verilator public_flat_rd*/;
    logic [1:0] htrans /*verilator public_flat_rd*/;
    logic [2:0] hsize /*verilator public_flat_rd*/;
    logic hready /*verilator public_flat_rd*/;
    logic [31:0] hwdata /*verilator public_flat_rd*/;
    logic [31:0] hrdata /*verilator public_flat_rd*/;
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

    // Address-phase capture; the data phase completes one cycle later,
    // or two for the system bus.
    logic dph_active /*verilator public_flat_rd*/;
    logic dph_write /*verilator public_flat_rd*/;
    logic dph_mmio /*verilator public_flat_rd*/;
    logic dph_ext /*verilator public_flat_rd*/;
    logic dph_waited /*verilator public_flat_rd*/;
    logic [14:0] dph_word;  // TCM word index; strb carries the byte lanes
    logic [31:0] dph_addr;
    logic [3:0] dph_strb;
    logic [4:0] mmio_reg;

    always_comb hready = !(dph_active && dph_ext && !dph_waited);

    logic [3:0] strb;
    always_comb begin
        case (hsize[1:0])
            2'b00: strb = 4'b0001 << haddr[1:0];
            2'b01: strb = haddr[1] ? 4'b1100 : 4'b0011;
            default: strb = 4'b1111;
        endcase
    end

    logic [31:0] tcm[TCM_WORDS] /*verilator public_flat_rw*/;

    generate
        if (TCM_INIT_FILE != "") begin : tcm_init
            initial $readmemh(TCM_INIT_FILE, tcm);
        end
    endgenerate

    logic [31:0] tcm_rdata /*verilator public_flat_rd*/;
    logic [14:0] word_addr;
    always_comb word_addr = haddr[16:2];

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            dph_active <= 1'b0;
            dph_write <= 1'b0;
            dph_mmio <= 1'b0;
            dph_ext <= 1'b0;
            dph_waited <= 1'b0;
            dph_word <= '0;
            dph_addr <= '0;
            dph_strb <= '0;
            mmio_reg <= '0;
        end else if (hready) begin
            dph_active <= htrans[1];
            dph_write <= hwrite;
            dph_mmio <= haddr[31:28] == 4'hF;
            dph_ext <= haddr[31:28] != 4'h0 && haddr[31:28] != 4'hF;
            dph_word <= haddr[16:2];
            dph_addr <= haddr;
            dph_strb <= strb;
            mmio_reg <= haddr[4:0];
            dph_waited <= 1'b0;
        end else if (rv_soc_bus_stb) begin
            dph_waited <= 1'b1;
        end
    end

    // The strobe is the wait-state cycle: captured address out, write data
    // straight off the held AHB data phase, reads answered next cycle.
    /* pend has no rdy term; its own block keeps the scheduler from seeing
     * a loop through the arbiter. */
    always_comb rv_soc_bus_pend = dph_active && dph_ext && !dph_waited;
    always_comb begin
        rv_soc_bus_stb = rv_soc_bus_pend && bus_rdy;
        rv_soc_bus_we = rv_soc_bus_stb && dph_write;
        rv_soc_bus_addr = dph_addr;
        rv_soc_bus_wdata = hwdata;
        rv_soc_bus_wstrb = dph_strb;
    end

    // TCM read launches in the address phase; write lands in the data phase.
    // The decode matters: an external-window write must not also land here,
    // or the loader overwrites the firmware under its own feet.
    always_ff @(posedge clk) begin
        tcm_rdata <= tcm[word_addr];
        if (dph_active && dph_write && !dph_mmio && !dph_ext) begin
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

    logic [7:0] mmio_kbd_data /*verilator public_flat_rw*/;
    logic mmio_kbd_valid /*verilator public_flat_rw*/;
    logic [8:0] mmio_key_data /*verilator public_flat_rw*/;
    logic mmio_key_valid /*verilator public_flat_rw*/;
    always_comb rv_soc_key_pending = mmio_key_valid;
    logic [31:0] mmio_slot_len /*verilator public_flat_rw*/;

    logic [63:0] mtime_us;
    logic [15:0] mtime_acc;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            mtime_us <= 64'd0;
            mtime_acc <= '0;
        end else if ({16'd0, mtime_acc} + 32'(MTIME_ADD) >= 32'(MTIME_WRAP))
        begin
            mtime_acc <= 16'(32'(mtime_acc) + 32'(MTIME_ADD)
                             - 32'(MTIME_WRAP));
            mtime_us <= mtime_us + 64'd1;
        end else begin
            mtime_acc <= mtime_acc + 16'(MTIME_ADD);
        end
    end

    always_comb begin
        if (dph_ext)
            hrdata = bus_rdata;
        else if (dph_mmio)
            case (mmio_reg)
                5'h08: hrdata = {23'd0, mmio_kbd_valid, mmio_kbd_data};
                5'h10: hrdata = mtime_us[31:0];
                5'h14: hrdata = mtime_us[63:32];
                5'h18: hrdata = mmio_slot_len;
                5'h1C: hrdata = {22'd0, mmio_key_valid, mmio_key_data};
                default: hrdata = 32'h0;
            endcase
        else
            hrdata = tcm_rdata;
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            rv_soc_tx_data <= 8'h00;
            rv_soc_tx_valid <= 1'b0;
            rv_soc_halted <= 1'b0;
            rv_soc_exit_code <= 32'h0;
            mmio_kbd_valid <= 1'b0;
            mmio_kbd_data <= 8'h00;
            mmio_key_valid <= 1'b0;
            mmio_key_data <= 9'h000;
            mmio_slot_len <= 32'h0;
        end else begin
            rv_soc_tx_valid <= 1'b0;
            if (dph_active && !dph_write && dph_mmio && mmio_reg == 5'h08)
                mmio_kbd_valid <= 1'b0;
            if (dph_active && !dph_write && dph_mmio && mmio_reg == 5'h1C)
                mmio_key_valid <= 1'b0;
            if (slot_set)
                mmio_slot_len <= slot_len;
            if (key_set) begin
                mmio_key_data <= key_code;
                mmio_key_valid <= 1'b1;
            end
            if (dph_active && dph_write && dph_mmio) begin
                case (mmio_reg)
                    5'h00: begin
                        rv_soc_tx_data <= hwdata[7:0];
                        rv_soc_tx_valid <= 1'b1;
                    end
                    5'h04: begin
                        rv_soc_halted <= 1'b1;
                        rv_soc_exit_code <= hwdata;
                    end
                    5'h18: mmio_slot_len <= hwdata;
                    default: ;
                endcase
            end
        end
    end

endmodule
