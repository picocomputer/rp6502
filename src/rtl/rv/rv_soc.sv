/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * A Hazard3, which is the RP2350's own core, so the firmware that runs
 * here is the firmware that runs there.
 *
 * The TCM and the local MMIO page answer inside the data phase. Anything
 * else costs a wait state, which is what gives the machine's
 * single-cycle devices one strobe per access.
 */

module rv_soc #(
    parameter int MTIME_ADD = 1,
    parameter int MTIME_WRAP = 1,
    parameter TCM_INIT_FILE = ""
) (
    input logic clk,
    input logic rst_n,

    input logic slot_set,
    input logic [31:0] slot_len,
    input logic [7:0] upd_n,
    input logic key_set,
    input logic [8:0] key_code,
    /* Levels, so no mailbox. A mouse's counter — how a new report is
     * told from a still hand — rides in its own key word, because the
     * deltas are the same zero either way. */
    input logic [3:0][31:0] cont_key,
    input logic [3:0][31:0] cont_joy,
    input logic [3:0][15:0] cont_trig,
    output logic rv_soc_key_pending,

    output logic [7:0] rv_soc_tx_data,
    output logic rv_soc_tx_valid,

    output logic [15:0] rv_soc_phi2_khz,

    output logic rv_soc_halted,
    output logic [31:0] rv_soc_exit_code,

    input logic bus_rdy,
    /* Told, not worked out again from bus_rdy: that term moves on the
     * machine's edge, which is this clock's edge, so a second answer
     * here differs from the first and the access happens twice or not
     * at all. */
    input logic bus_taken,
    output logic rv_soc_bus_pend,
    output logic rv_soc_bus_stb,
    output logic rv_soc_bus_we,
    output logic [31:0] rv_soc_bus_addr,
    output logic [31:0] rv_soc_bus_wdata,
    output logic [3:0] rv_soc_bus_wstrb,
    input logic [31:0] bus_rdata
);

    localparam int TCM_WORDS = 24576;  // 96 KB
    localparam int TCM_AW = $clog2(TCM_WORDS);

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

    logic dph_active /*verilator public_flat_rd*/;
    logic dph_write /*verilator public_flat_rd*/;
    logic dph_mmio /*verilator public_flat_rd*/;
    logic dph_ext /*verilator public_flat_rd*/;
    logic dph_waited /*verilator public_flat_rd*/;
    logic [TCM_AW-1:0] dph_word;  // TCM word; strb carries the lanes
    logic [31:0] dph_addr;
    logic [3:0] dph_strb;
    logic [6:0] mmio_reg;

    always_comb hready = !(dph_active && dph_ext && !dph_waited);

    logic [3:0] strb;
    always_comb begin
        case (hsize[1:0])
            2'b00: strb = 4'b0001 << haddr[1:0];
            2'b01: strb = haddr[1] ? 4'b1100 : 4'b0011;
            default: strb = 4'b1111;
        endcase
    end

    /* One array per byte lane. A byte-enabled write keeps a dual-port
     * RAM from being inferred at all, and a megabit of code memory
     * built from flip-flops does not fit in any device made. */
    (* ramstyle = "no_rw_check" *)
    logic [7:0] tcm0[TCM_WORDS] /*verilator public_flat_rw*/;
    (* ramstyle = "no_rw_check" *)
    logic [7:0] tcm1[TCM_WORDS] /*verilator public_flat_rw*/;
    (* ramstyle = "no_rw_check" *)
    logic [7:0] tcm2[TCM_WORDS] /*verilator public_flat_rw*/;
    (* ramstyle = "no_rw_check" *)
    logic [7:0] tcm3[TCM_WORDS] /*verilator public_flat_rw*/;

    generate
        if (TCM_INIT_FILE != "") begin : tcm_init
            initial begin
                $readmemh({TCM_INIT_FILE, ".0"}, tcm0);
                $readmemh({TCM_INIT_FILE, ".1"}, tcm1);
                $readmemh({TCM_INIT_FILE, ".2"}, tcm2);
                $readmemh({TCM_INIT_FILE, ".3"}, tcm3);
            end
        end
    endgenerate

    logic [31:0] tcm_rdata /*verilator public_flat_rd*/;
    logic [TCM_AW-1:0] word_addr;
    always_comb word_addr = haddr[TCM_AW+1:2];

    /* A store's data phase overlaps the next load's address phase, so a
     * load of the word just stored samples the array on the same edge
     * the write lands and reads what was there before. The compiler
     * emits that pair, and the M10K is told no_rw_check, so these carry
     * the write around. */
    logic [3:0] tcm_fwd;
    logic [31:0] tcm_fwd_data;
    logic [31:0] tcm_q;
    logic tcm_wr;
    always_comb tcm_wr = dph_active && dph_write && !dph_mmio && !dph_ext;

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
            dph_word <= haddr[TCM_AW+1:2];
            dph_addr <= haddr;
            dph_strb <= strb;
            mmio_reg <= haddr[6:0];
            dph_waited <= 1'b0;
        end else if (bus_taken) begin
            dph_waited <= 1'b1;
        end
    end

    /* pend has no rdy term; its own block keeps the scheduler from seeing
     * a loop through the arbiter. */
    always_comb rv_soc_bus_pend = dph_active && dph_ext && !dph_waited;
    always_comb begin
        rv_soc_bus_stb = rv_soc_bus_pend && bus_rdy;
        /* The data phase's alone. Qualified with the strobe it carried
         * bus_rdy, answering live where the strobe answers from the
         * falling edge, and a write that lands as a read is gone. */
        rv_soc_bus_we = dph_write;
        rv_soc_bus_addr = dph_addr;
        rv_soc_bus_wdata = hwdata;
        rv_soc_bus_wstrb = dph_strb;
    end

    /* The decode matters: an external-window write must not also land
     * here, or the loader overwrites the firmware under its own feet. */
    always_ff @(posedge clk) begin
        tcm_rdata <= {tcm3[word_addr], tcm2[word_addr],
                      tcm1[word_addr], tcm0[word_addr]};
        tcm_fwd <= (tcm_wr && dph_word == word_addr) ? dph_strb : 4'b0000;
        tcm_fwd_data <= hwdata;
        if (tcm_wr) begin
            if (dph_strb[0])
                tcm0[dph_word] <= hwdata[7:0];
            if (dph_strb[1])
                tcm1[dph_word] <= hwdata[15:8];
            if (dph_strb[2])
                tcm2[dph_word] <= hwdata[23:16];
            if (dph_strb[3])
                tcm3[dph_word] <= hwdata[31:24];
        end
    end

    logic [7:0] mmio_kbd_data /*verilator public_flat_rw*/;
    logic mmio_kbd_valid /*verilator public_flat_rw*/;
    logic [8:0] mmio_key_data /*verilator public_flat_rw*/;
    logic mmio_key_valid /*verilator public_flat_rw*/;
    always_comb rv_soc_key_pending = mmio_key_valid;
    logic [31:0] mmio_slot_len /*verilator public_flat_rw*/;

    logic [63:0] mtime_us /*verilator public_flat_rd*/;
    logic [15:0] mtime_acc;
    initial begin
        mtime_us = 64'd0;
        mtime_acc = '0;
    end
    always_ff @(posedge clk) begin
        if ({16'd0, mtime_acc} + 32'(MTIME_ADD) >= 32'(MTIME_WRAP))
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
                7'h08: hrdata = {23'd0, mmio_kbd_valid, mmio_kbd_data};
                7'h0C: hrdata = {16'd0, rv_soc_phi2_khz};
                7'h10: hrdata = mtime_us[31:0];
                7'h14: hrdata = mtime_us[63:32];
                7'h18: hrdata = mmio_slot_len;
                7'h4C: hrdata = {24'd0, upd_n};
                7'h1C: hrdata = {22'd0, mmio_key_valid, mmio_key_data};
                /* The controller slots, three words each in APF's own
                 * order. Written out rather than indexed because the
                 * twelve-byte stride is not a bit slice. */
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
            hrdata = tcm_q;
    end

    always_comb begin
        tcm_q[7:0] = tcm_fwd[0] ? tcm_fwd_data[7:0] : tcm_rdata[7:0];
        tcm_q[15:8] = tcm_fwd[1] ? tcm_fwd_data[15:8] : tcm_rdata[15:8];
        tcm_q[23:16] = tcm_fwd[2] ? tcm_fwd_data[23:16] : tcm_rdata[23:16];
        tcm_q[31:24] = tcm_fwd[3] ? tcm_fwd_data[31:24] : tcm_rdata[31:24];
    end

    initial begin
        rv_soc_tx_data = 8'h00;
        rv_soc_tx_valid = 1'b0;
        rv_soc_halted = 1'b0;
        rv_soc_exit_code = 32'h0;
        mmio_kbd_valid = 1'b0;
        mmio_kbd_data = 8'h00;
        mmio_key_valid = 1'b0;
        mmio_key_data = 9'h000;
        mmio_slot_len = 32'h0;
        /* The machine runs at its fastest until told otherwise, so a
         * firmware that never sets it still gets the default. */
        rv_soc_phi2_khz = 16'd8000;
    end
    always_ff @(posedge clk) begin
            rv_soc_tx_valid <= 1'b0;
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
                        rv_soc_tx_data <= hwdata[7:0];
                        rv_soc_tx_valid <= 1'b1;
                    end
                    7'h04: begin
                        rv_soc_halted <= 1'b1;
                        rv_soc_exit_code <= hwdata;
                    end
                    7'h0C: rv_soc_phi2_khz <= hwdata[15:0];
                    7'h18: mmio_slot_len <= hwdata;
                    default: ;
                endcase
            end
    end

endmodule
