/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The RIA register window at $FFE0-$FFFF. The emulator defines the
 * semantics and this must match it — every read side effect below is
 * there because emu/sys/ria.c has it, not because the fabric wants it.
 *
 * Read data is combinational from pre-tick state; side effects land at
 * the enabled edge.
 *
 * The RW ports read and write XRAM at their address registers, and the
 * cells at $FFE4/$FFE8 are staging mirrors the engine keeps loaded — a
 * background refresh when the port is free, an urgent restage after
 * every access — so a writer to XRAM heals the stage before the next
 * PHI2 and the 6502's combinational read is still the emulator's.
 *
 * A write to $FFEF arms the BRA -2 block at $FFF1 in the same cycle the
 * op lands, so the 6502 can JSR into the trampoline immediately and
 * never outruns the OS.
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

    output logic [7:0] ria_regs_tx_data,
    output logic ria_regs_tx_valid,

    input logic rx_valid,
    input logic [7:0] rx_data,
    output logic ria_regs_rx_taken,

    input logic vsync_pulse,
    output logic ria_regs_irq,

    /* The engine owns port B while busy; the background refresh yields
     * to the soft CPU. */
    output logic ria_regs_xr_busy,
    output logic ria_regs_xr_we,
    output logic [15:0] ria_regs_xr_addr,
    output logic [7:0] ria_regs_xr_wdata,
    input logic [7:0] xr_rdata,
    input logic xr_cpu_want,

    /* The OS side. The RX ask is the emulator's lazy pull made visible:
     * it is served exactly once and never remembered into a later byte's
     * arrival, so the OS never commits a byte the console still wants. */
    input logic b_we,
    input logic b_re,
    input logic [7:0] b_word,
    input logic [3:0] b_wstrb,
    input logic [31:0] b_wdata,
    output logic [31:0] ria_regs_b_rdata,

    output logic ria_regs_api_pending,
    input logic api_ack
);

    localparam logic [7:0] TX_READY = 8'h80;
    localparam logic [7:0] RX_READY = 8'h40;

    logic [7:0] regs[32] /*verilator public_flat_rw*/;

    /* Acks resolve before this frame's pulse, so a pulse is never lost
     * to a same-edge acknowledge. */
    logic [7:0] irq_pending /*verilator public_flat_rd*/;
    logic [7:0] irq_enabled /*verilator public_flat_rd*/;
    logic [7:0] pend_next;
    always_comb begin
        pend_next = irq_pending;
        if (en && cs && !we && rs == 5'h10)
            pend_next = 8'h00;
        if (en && cs && we && rs == 5'h10)
            pend_next = pend_next & ~data_i;
        if (vsync_pulse)
            pend_next = pend_next | 8'h80;
    end
    always_comb ria_regs_irq = (irq_pending & irq_enabled) != 8'h00;

    /* Sixteen bytes against an M10K's 10,240: left alone Quartus spends
     * a whole block on 128 bits and adds pass-through registers, because
     * the read below is asynchronous and a block's read address is not.
     * An MLAB answers where it is used. */
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [7:0] txf[16];
    logic [3:0] txf_w, txf_r;
    logic [4:0] txf_count;
    logic txf_pop;
    logic push_now;
    always_comb push_now = en && cs && we && rs == 5'h01 && txf_count < 5'd16;

    logic xr_wr_pend;
    logic [15:0] xr_wr_addr;
    logic [7:0] xr_wr_byte;
    logic xr_fill_pend0, xr_fill_pend1;
    logic xr_bg_alt;
    logic xr_cap0, xr_cap1;  // xr_rdata belongs to RW0/RW1 this clock

    logic xr_bg_go, xr_issue_f0, xr_issue_f1;
    always_comb begin
        xr_bg_go = !xr_wr_pend && !xr_fill_pend0 && !xr_fill_pend1
            && !xr_cpu_want;
        xr_issue_f0 = !xr_wr_pend && (xr_fill_pend0 || (xr_bg_go && !xr_bg_alt));
        xr_issue_f1 = !xr_wr_pend && !xr_fill_pend0
            && (xr_fill_pend1 || (xr_bg_go && xr_bg_alt));
        ria_regs_xr_busy = xr_wr_pend || xr_fill_pend0 || xr_fill_pend1
            || xr_bg_go;
        ria_regs_xr_we = xr_wr_pend;
        ria_regs_xr_wdata = xr_wr_byte;
        if (xr_wr_pend)
            ria_regs_xr_addr = xr_wr_addr;
        else if (xr_issue_f0)
            ria_regs_xr_addr = {regs[5'h07], regs[5'h06]};
        else
            ria_regs_xr_addr = {regs[5'h0B], regs[5'h0A]};
    end

    /* LUT RAM is one write port and one asynchronous read port, so the
     * bytes are one array per lane and the set exists twice — the OS's
     * word read and the pop's mirror refill are two readers. The guard
     * stays registers so the arrays keep a clean 128 deep. */
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [7:0] xs0[128];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [7:0] xs1[128];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [7:0] xs2[128];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [7:0] xs3[128];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [7:0] xm0[128];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [7:0] xm1[128];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [7:0] xm2[128];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [7:0] xm3[128];
    logic [31:0] xs_guard;
    logic [9:0] xsp;
    logic xs_fill;  // a pop's mirror refill lands one clock later
    logic [9:0] xs_fill_at;

    logic os_rx_valid;
    logic [7:0] os_rx_data;
    logic rx_req;
    logic eff_rx_valid;
    logic [7:0] eff_rx_data;
    always_comb begin
        eff_rx_valid = rx_valid || os_rx_valid;
        eff_rx_data = rx_valid ? rx_data : os_rx_data;
    end

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

    /* Registered with the side effect it reports: the offered byte is
     * taken at the same edge that latches it, never a cycle before. */
    logic [7:0] xs_word;
    logic xs_win;
    always_comb begin
        xs_word = b_word - 8'd64;
        xs_win = b_word >= 8'd64 && b_word <= 8'd192;
    end

    logic xs_push;
    logic [8:0] xs_push_at;
    logic [3:0] xs_os_lane, xs_we;
    logic [6:0] xs_waddr0, xs_waddr1, xs_waddr2, xs_waddr3;
    logic [7:0] xs_wdat0, xs_wdat1, xs_wdat2, xs_wdat3;
    always_comb begin
        xs_push = en && cs && we && rs == 5'h0C && xsp != 10'd0;
        xs_push_at = 9'(xsp - 10'd1);
        xs_os_lane = {4{b_we && xs_win && !xs_word[7]}} & b_wstrb;
        xs_we = xs_os_lane | ({3'd0, xs_push} << xs_push_at[1:0]);
        xs_waddr0 = xs_os_lane[0] ? xs_word[6:0] : xs_push_at[8:2];
        xs_waddr1 = xs_os_lane[1] ? xs_word[6:0] : xs_push_at[8:2];
        xs_waddr2 = xs_os_lane[2] ? xs_word[6:0] : xs_push_at[8:2];
        xs_waddr3 = xs_os_lane[3] ? xs_word[6:0] : xs_push_at[8:2];
        xs_wdat0 = xs_os_lane[0] ? b_wdata[7:0] : data_i;
        xs_wdat1 = xs_os_lane[1] ? b_wdata[15:8] : data_i;
        xs_wdat2 = xs_os_lane[2] ? b_wdata[23:16] : data_i;
        xs_wdat3 = xs_os_lane[3] ? b_wdata[31:24] : data_i;
    end

    logic [7:0] xs_fill_byte;
    always_comb begin
        case (xs_fill_at[1:0])
            2'd0: xs_fill_byte = xm0[xs_fill_at[8:2]];
            2'd1: xs_fill_byte = xm1[xs_fill_at[8:2]];
            2'd2: xs_fill_byte = xm2[xs_fill_at[8:2]];
            default: xs_fill_byte = xm3[xs_fill_at[8:2]];
        endcase
        if (xs_fill_at[9])
            xs_fill_byte = xs_guard[7:0];
    end

    /* Outside the reset: an array an asynchronous reset can reach
     * infers as flip-flops, never as memory. */
    always_ff @(posedge clk) begin
        if (push_now)
            txf[txf_w] <= data_i;
        if (xs_we[0]) begin
            xs0[xs_waddr0] <= xs_wdat0;
            xm0[xs_waddr0] <= xs_wdat0;
        end
        if (xs_we[1]) begin
            xs1[xs_waddr1] <= xs_wdat1;
            xm1[xs_waddr1] <= xs_wdat1;
        end
        if (xs_we[2]) begin
            xs2[xs_waddr2] <= xs_wdat2;
            xm2[xs_waddr2] <= xs_wdat2;
        end
        if (xs_we[3]) begin
            xs3[xs_waddr3] <= xs_wdat3;
            xm3[xs_waddr3] <= xs_wdat3;
        end
        if (b_we && xs_win && xs_word[7]) begin
            if (b_wstrb[0])
                xs_guard[7:0] <= b_wdata[7:0];
            if (b_wstrb[1])
                xs_guard[15:8] <= b_wdata[15:8];
            if (b_wstrb[2])
                xs_guard[23:16] <= b_wdata[23:16];
            if (b_wstrb[3])
                xs_guard[31:24] <= b_wdata[31:24];
        end
    end

    always_comb begin
        case (b_word)
            8'd16: ria_regs_b_rdata = {23'd0, txf_count != 5'd0, txf[txf_r]};
            8'd18: ria_regs_b_rdata = {30'd0, rx_req, !os_rx_valid};
            8'd200: ria_regs_b_rdata = {22'd0, xsp};
            default: begin
                if (xs_win)
                    ria_regs_b_rdata = xs_word[7] ? xs_guard : {
                        xs3[xs_word[6:0]], xs2[xs_word[6:0]],
                        xs1[xs_word[6:0]], xs0[xs_word[6:0]]
                    };
                else
                    ria_regs_b_rdata = {
                        regs[{b_word[2:0], 2'd3}], regs[{b_word[2:0], 2'd2}],
                        regs[{b_word[2:0], 2'd1}], regs[{b_word[2:0], 2'd0}]
                    };
            end
        endcase
    end

    always_comb txf_pop = b_re && b_word == 8'd16 && txf_count != 5'd0;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            for (int i = 0; i < 32; i++)
                regs[i] <= 8'h00;
            ria_regs_tx_data <= 8'h00;
            ria_regs_tx_valid <= 1'b0;
            ria_regs_rx_taken <= 1'b0;
            ria_regs_api_pending <= 1'b0;
            irq_pending <= 8'h00;
            irq_enabled <= 8'h00;
        end else begin
            if (en) begin
                ria_regs_tx_valid <= 1'b0;
                ria_regs_rx_taken <= pull;
                if (api_ack)
                    ria_regs_api_pending <= 1'b0;
                if (cs) begin
                    if (we) begin
                        case (rs)
                            5'h01: begin
                                /* A write while full drops; TX_READY told
                                 * the program not to. */
                                ria_regs_tx_data <= data_i;
                                ria_regs_tx_valid <= 1'b1;
                            end
                            5'h04: begin
                                /* The byte goes to XRAM, never the cell; the
                                 * address steps past it. */
                                {regs[5'h07], regs[5'h06]} <=
                                    {regs[5'h07], regs[5'h06]}
                                    + {{8{regs[5'h05][7]}}, regs[5'h05]};
                            end
                            5'h08: begin
                                {regs[5'h0B], regs[5'h0A]} <=
                                    {regs[5'h0B], regs[5'h0A]}
                                    + {{8{regs[5'h09][7]}}, regs[5'h09]};
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
                            5'h10: ;  /* enable/ack handled above the case */
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
                            5'h04: begin
                                /* The staged byte answered; step the address. */
                                {regs[5'h07], regs[5'h06]} <=
                                    {regs[5'h07], regs[5'h06]}
                                    + {{8{regs[5'h05][7]}}, regs[5'h05]};
                            end
                            5'h08: begin
                                {regs[5'h0B], regs[5'h0A]} <=
                                    {regs[5'h0B], regs[5'h0A]}
                                    + {{8{regs[5'h09][7]}}, regs[5'h09]};
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
            /* The frame counter and the $FFF0 block land every
             * clock: pending resolves through pend_next, its
             * regs cell is only the mirror. */
                if (vsync_pulse)
                    regs[5'h03] <= regs[5'h03] + 8'd1;
                if (en && cs && we && rs == 5'h10)
                    irq_enabled <= data_i;
                irq_pending <= pend_next;
                regs[5'h10] <= pend_next;
            /* The RW stages reload one clock after their issue; a same-edge
             * 6502 or OS write is repaired by the restage that follows it. */
            if (xr_cap0)
                regs[5'h04] <= xr_rdata;
            if (xr_cap1)
                regs[5'h08] <= xr_rdata;
            /* A pop's mirror refill arrives from the RAM one clock after the
             * pointer moved, always between 6502 cycles; an OS write below still
             * outranks it. */
            if (xs_fill)
                regs[5'h0C] <= xs_fill_byte;
            /* The OS side is plain shared memory at the system clock; it lands
             * regardless of the 6502's enable, and a same-cell collision goes to
             * the OS, as arbitrary as it is on the real dual-core part. */
            if (b_we && b_word < 8'd8) begin
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
    end

    /* The rings live at the system clock: the 6502 pushes on its enable, the
     * OS pops and offers whenever its strobe lands. The push's data lands
     * in the reset-free block above; only the pointer belongs here. */
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            txf_w <= 4'd0;
            txf_r <= 4'd0;
            txf_count <= 5'd0;
            os_rx_valid <= 1'b0;
            os_rx_data <= 8'h00;
            rx_req <= 1'b0;
            xsp <= 10'd512;
            xs_fill <= 1'b0;
            xs_fill_at <= 10'd0;
            xr_wr_pend <= 1'b0;
            xr_wr_addr <= 16'h0000;
            xr_wr_byte <= 8'h00;
            xr_fill_pend0 <= 1'b0;
            xr_fill_pend1 <= 1'b0;
            xr_bg_alt <= 1'b0;
            xr_cap0 <= 1'b0;
            xr_cap1 <= 1'b0;
        end else begin
            /* The engine: retire what issued this clock, then take the
             * 6502's access. A write-back holds the pre-step address. */
            xr_cap0 <= xr_issue_f0;
            xr_cap1 <= xr_issue_f1;
            if (xr_wr_pend)
                xr_wr_pend <= 1'b0;
            if (xr_issue_f0)
                xr_fill_pend0 <= 1'b0;
            if (xr_issue_f1)
                xr_fill_pend1 <= 1'b0;
            if (xr_bg_go)
                xr_bg_alt <= !xr_bg_alt;
            if (en && cs && rs == 5'h04) begin
                if (we) begin
                    xr_wr_pend <= 1'b1;
                    xr_wr_addr <= {regs[5'h07], regs[5'h06]};
                    xr_wr_byte <= data_i;
                end
                xr_fill_pend0 <= 1'b1;
            end
            if (en && cs && rs == 5'h08) begin
                if (we) begin
                    xr_wr_pend <= 1'b1;
                    xr_wr_addr <= {regs[5'h0B], regs[5'h0A]};
                    xr_wr_byte <= data_i;
                end
                xr_fill_pend1 <= 1'b1;
            end
            /* The 6502's stack ops, and the pop's one-clock-late refill of
             * the top-of-stack mirror out of the RAM. */
            xs_fill <= 1'b0;
            if (en && cs && rs == 5'h0C) begin
                if (we) begin
                    if (xsp != 10'd0)
                        xsp <= xsp - 10'd1;
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
            /* The xstack bytes land in their own block above; the
             * pointer is the OS's other door. */
            if (b_we && b_word == 8'd200)
                xsp <= b_wdata[9:0];
            if (push_now)
                txf_w <= txf_w + 4'd1;
            if (txf_pop)
                txf_r <= txf_r + 4'd1;
            txf_count <= txf_count + {4'd0, push_now} - {4'd0, txf_pop};
            /* An unanswered ask arms the request; a landed byte, whether
             * pulled or offered, satisfies it. */
            if (en && cs && !we && !pull
                && ((rs == 5'h00 && (regs[0] & RX_READY) == 8'h00)
                    || rs == 5'h02))
                rx_req <= 1'b1;
            if (en && pull)
                rx_req <= 1'b0;
            if (b_we && b_word == 8'd18 && b_wdata[9])
                rx_req <= 1'b0;
            else if (b_we && b_word == 8'd18 && !os_rx_valid) begin
                os_rx_valid <= 1'b1;
                os_rx_data <= b_wdata[7:0];
                rx_req <= 1'b0;
            end else if (en && pull && !rx_valid) begin
                os_rx_valid <= 1'b0;
            end
        end
    end

endmodule
