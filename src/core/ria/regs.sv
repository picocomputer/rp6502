/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The RIA register window at $FFE0-$FFFF. The emulator defines the
 * semantics and this must match it — every side effect of a 6502 read
 * below is there because core/ria/ria.c has it.
 *
 * Read data is combinational from pre-tick state; side effects land at
 * the enabled edge.
 *
 * The cells at $FFE4/$FFE8 are staging mirrors the engine keeps loaded —
 * a background refresh when the port is free, an urgent restage after
 * every access — so that a combinational 6502 read of either cell returns
 * the byte the emulator's read returns.
 *
 * A write to $FFEF arms the BRA -2 block at $FFF1 in the same cycle the
 * op lands, so the 6502 can JSR into the trampoline immediately. Two ops
 * are done there and then, as the other machines do them inside the
 * write: DROP ($00) empties the xstack and releases, so the 6502 can push
 * at once, and EXIT ($FF) holds the 6502 until the OS has it in reset.
 */

module regs (
    input logic clk,
    input logic en,

    input logic cs,
    input logic we,
    input logic [4:0] rs,
    input logic [7:0] data_i,

    output logic [7:0] regs_data,

    output logic [7:0] regs_tx_data,
    output logic regs_tx_valid,

    input logic rx_valid,
    input logic [7:0] rx_data,
    output logic regs_rx_taken,

    input logic vsync_pulse,
    output logic regs_irq,

    /* The engine owns port B while busy; the background refresh yields
     * to the soft CPU. */
    output logic regs_xr_busy,
    output logic regs_xr_we,
    output logic [15:0] regs_xr_addr,
    output logic [7:0] regs_xr_wdata,
    input logic [7:0] xr_rdata,
    input logic xr_cpu_want,

    /* The RX request is cleared when the OS serves it, with a byte or
     * with nothing, rather than held until a later byte arrives. A byte
     * that arrives after the request is served is not offered to the
     * 6502 until another 6502 read sets the request. A read of word 26
     * takes back the byte the 6502 has not read. A write of word 27
     * sets SIGINT and a read of it clears SIGINT. */
    input logic b_we,
    input logic b_re,
    input logic [7:0] b_word,
    input logic [3:0] b_wstrb,
    input logic [31:0] b_wdata,

    /* sst_restore has the window's writes put back what its reads gave a
     * savestate, where a plain write is the OS's own: word 18 writes the
     * RX state and the API flag instead of offering a byte, word 20 writes
     * the console queue's pointers, and word 200 also refills $FFEC's
     * mirror. A savestate never reads word 16, 26 or 27, whose reads
     * take bytes or SIGINT. */
    input logic sst_restore,
    output logic [31:0] regs_b_rdata,

    output logic regs_api_pending,
    input logic api_ack,

    /* High from an EXIT until the OS releases it, by which time the 6502
     * is in reset. It stalls the 6502's next opcode fetch. */
    output logic regs_hold
);

    localparam logic [7:0] TX_READY = 8'h80;
    localparam logic [7:0] RX_READY = 8'h40;

    logic [7:0] regs[32] /*verilator public_flat_rw*/;

    /* Acks resolve before this frame's pulse, so a pulse is never lost
     * to a same-edge acknowledge. SIGINT is bit 6 of the same pending
     * byte, so RIA_ATTR_SIGINT and the interrupt are one latch, as on the
     * other machines: the OS sets it with a write of word 27 for a Ctrl-C
     * and takes it with a read of word 27 for the attribute, and the reset
     * of word 17 at the start of a ROM clears it with the rest. */
    logic [7:0] irq_pending /*verilator public_flat_rd*/;
    logic [7:0] irq_enabled /*verilator public_flat_rd*/;
    logic [7:0] pend_next;
    logic sig_set, sig_take;
    always_comb begin
        sig_set = b_we && b_word == 8'd27;
        sig_take = b_re && b_word == 8'd27;
        pend_next = irq_pending;
        if (en && cs && !we && rs == 5'h10)
            pend_next = 8'h00;
        if (en && cs && we && rs == 5'h10)
            pend_next = pend_next & ~data_i;
        if (sig_take)
            pend_next = pend_next & ~8'h40;
        if (vsync_pulse)
            pend_next = pend_next | 8'h80;
        if (sig_set)
            pend_next = pend_next | 8'h40;
    end
    always_comb regs_irq = (irq_pending & irq_enabled) != 8'h00;

    /* Flops, not an MLAB. The savestate reads the whole queue at once
     * and writes the whole queue back, which is five more ports than a
     * memory has: sixteen bytes of the 6502's console output that the
     * soft CPU had not taken yet, and which a sleep used to drop on the
     * floor. */
    logic [7:0] txf[16];
    /* txf_wr and txf_rd only count up, a byte's slot is its pointer's low
     * four bits, and txf_len is the count. */
    logic [4:0] txf_wr, txf_rd, txf_len;
    always_comb txf_len = txf_wr - txf_rd;
    logic txf_pop;
    logic push_now;
    always_comb push_now = en && cs && we && rs == 5'h01
        && txf_len < 5'd16;

    /* The console queue's four words in the regs window, and the fifth
     * that carries its pointers. Read here and nowhere else: word 16 is
     * the queue's front door and reading it takes the byte off, so the
     * savestate cannot use it. */
    localparam int TXQ_W0 = 21;
    localparam int TXQ_PTR = 20;
    logic txq_win;
    logic [1:0] txq_row;
    always_comb begin
        txq_win = b_word >= 8'(TXQ_W0) && b_word < 8'(TXQ_W0 + 4);
        txq_row = 2'(b_word - 8'(TXQ_W0));
    end

    logic xr_wr_pend;
    logic [15:0] xr_wr_addr;
    logic [7:0] xr_wr_byte;
    logic xr_fill_pend0, xr_fill_pend1;
    logic xr_bg_alt;
    logic xr_cap0, xr_cap1;

    logic xr_bg_go, xr_issue_f0, xr_issue_f1;
    always_comb begin
        xr_bg_go = !xr_wr_pend && !xr_fill_pend0 && !xr_fill_pend1
            && !xr_cpu_want;
        xr_issue_f0 = !xr_wr_pend && (xr_fill_pend0 || (xr_bg_go && !xr_bg_alt));
        xr_issue_f1 = !xr_wr_pend && !xr_fill_pend0
            && (xr_fill_pend1 || (xr_bg_go && xr_bg_alt));
        regs_xr_busy = xr_wr_pend || xr_fill_pend0 || xr_fill_pend1
            || xr_bg_go;
        regs_xr_we = xr_wr_pend;
        regs_xr_wdata = xr_wr_byte;
        if (xr_wr_pend)
            regs_xr_addr = xr_wr_addr;
        else if (xr_issue_f0)
            regs_xr_addr = {regs[5'h07], regs[5'h06]};
        else
            regs_xr_addr = {regs[5'h0B], regs[5'h0A]};
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

    logic os_rx_valid /*verilator public_flat_rd*/;
    logic [7:0] os_rx_data;
    logic rx_req /*verilator public_flat_rd*/;
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

    /* The byte the 6502 has not read. On the OS's path it is either in
     * the offer slot or latched in $FFE2 behind RX_READY, never both,
     * because the OS offers only on a request and only a read that leaves
     * the latch empty sets one. A $FFE2 read on this edge hands the latched
     * byte to the 6502, so it is not offered back. */
    logic rx_back_valid;
    logic [7:0] rx_back_data;
    logic rx_reclaim;
    always_comb begin
        rx_back_valid = os_rx_valid
            || ((regs[0] & RX_READY) != 8'h00
                && !(en && cs && !we && rs == 5'h02));
        rx_back_data = os_rx_valid ? os_rx_data : regs[2];
        rx_reclaim = b_re && b_word == 8'd26;
    end

    always_comb begin
        regs_data = regs[rs];
        if (cs && !we) begin
            case (rs)
                5'h00: regs_data = (regs[0] & ~TX_READY)
                    | (txf_len < 5'd16 ? TX_READY : 8'h00)
                    | (pull ? RX_READY : 8'h00);
                5'h02: regs_data = regs[2];
                default: ;
            endcase
        end
    end


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
            txf[txf_wr[3:0]] <= data_i;
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
        if (b_we && txq_win)
            for (int k = 0; k < 4; k++)
                if (b_wstrb[k])
                    txf[{txq_row, 2'(k)}] <= b_wdata[8 * k +: 8];
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
            8'd16: regs_b_rdata = {23'd0, txf_len != 5'd0, txf[txf_rd[3:0]]};
            /* The console queue, whole, for the one reader that must
             * not disturb it. */
            8'(TXQ_PTR): regs_b_rdata = {22'd0, txf_rd, txf_wr};
            8'(TXQ_W0 + 0): regs_b_rdata =
                {txf[3], txf[2], txf[1], txf[0]};
            8'(TXQ_W0 + 1): regs_b_rdata =
                {txf[7], txf[6], txf[5], txf[4]};
            8'(TXQ_W0 + 2): regs_b_rdata =
                {txf[11], txf[10], txf[9], txf[8]};
            8'(TXQ_W0 + 3): regs_b_rdata =
                {txf[15], txf[14], txf[13], txf[12]};
            /* The interrupt enable has no other way out: the 6502
             * writes $FFF0 and nothing reads it back, and the pending
             * cell in regs is a mirror rewritten every clock. A
             * savestate needs both, so they share a word. Word 27 reads
             * the same, and its read clears SIGINT. */
            8'd17, 8'd27: regs_b_rdata = {16'd0, irq_pending, irq_enabled};
            /* Bit 2 is here for the savestate and nothing else: a
             * state taken mid-API-call has to carry the pending flag,
             * or the restored 6502 spins in its trampoline waiting on
             * a call the soft CPU was never told about. */
            8'd18: regs_b_rdata = {16'd0, os_rx_data, 5'd0,
                                       regs_api_pending, rx_req,
                                       !os_rx_valid};
            /* Word 25 peeks at the byte the 6502 has not read, and a
             * read of word 26 also takes it back. */
            8'd25, 8'd26: regs_b_rdata = {23'd0, rx_back_valid,
                                           rx_back_data};
            8'd200: regs_b_rdata = {22'd0, xsp};
            default: begin
                if (xs_win)
                    regs_b_rdata = xs_word[7] ? xs_guard : {
                        xs3[xs_word[6:0]], xs2[xs_word[6:0]],
                        xs1[xs_word[6:0]], xs0[xs_word[6:0]]
                    };
                else
                    regs_b_rdata = {
                        regs[{b_word[2:0], 2'd3}], regs[{b_word[2:0], 2'd2}],
                        regs[{b_word[2:0], 2'd1}], regs[{b_word[2:0], 2'd0}]
                    };
            end
        endcase
    end

    always_comb txf_pop = b_re && b_word == 8'd16 && txf_len != 5'd0;

    always_comb regs_hold = regs[5'h0F] == 8'hFF && regs[5'h12][7];


    initial begin
        for (int i = 0; i < 32; i++)
            regs[i] = 8'h00;
        regs_tx_data = 8'h00;
        regs_tx_valid = 1'b0;
        regs_rx_taken = 1'b0;
        regs_api_pending = 1'b0;
        irq_pending = 8'h00;
        irq_enabled = 8'h00;
    end
    /* The 6502's side: every branch below is under `en`, the 6502's
     * enable. */
    always_ff @(posedge clk) begin
        if (en) begin
            regs_tx_valid <= 1'b0;
            regs_rx_taken <= pull;
            if (api_ack)
                regs_api_pending <= 1'b0;
            if (cs) begin
                if (we) begin
                    case (rs)
                        5'h01: begin
                            /* A write while full drops; TX_READY told
                             * the program not to. */
                            regs_tx_data <= data_i;
                            regs_tx_valid <= 1'b1;
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
                            if (xsp != 10'd0)
                                regs[5'h0C] <= data_i;
                        end
                        5'h0F: begin
                            /* The op lands and the trampoline blocks, one
                             * indivisible cycle. DROP blocks nothing: it
                             * returns 0 with the stack's guard byte on
                             * top, as api_return_ax(0) does. */
                            regs[5'h0F] <= data_i;
                            regs[5'h11] <= 8'h80;
                            if (data_i == 8'h00) begin
                                regs[5'h0C] <= xs_guard[7:0];
                                regs[5'h12] <= 8'h00;
                                regs[5'h13] <= 8'hA9;
                                regs[5'h14] <= 8'h00;
                                regs[5'h15] <= 8'hA2;
                                regs[5'h16] <= 8'h00;
                                regs[5'h17] <= 8'h60;
                            end else begin
                                regs[5'h12] <= 8'hFE;
                                regs_api_pending <= 1'b1;
                            end
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
            regs_tx_valid <= 1'b0;
            regs_rx_taken <= 1'b0;
            if (api_ack)
                regs_api_pending <= 1'b0;
        end
        /* After the 6502's access, so a byte its read moved from the offer
         * slot into the latch on this edge is taken back too. */
        if (rx_reclaim) begin
            regs[0] <= regs[0] & ~RX_READY;
            regs[2] <= 8'h00;
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
            /* Last, so a restore beats the resolution above rather than
             * being overwritten by it on the same edge. */
            if (b_we && b_word == 8'd17) begin
                if (b_wstrb[0])
                    irq_enabled <= b_wdata[7:0];
                if (b_wstrb[1])
                    irq_pending <= b_wdata[15:8];
            end
            if (sst_restore && b_we && b_word == 8'd18)
                regs_api_pending <= b_wdata[2];
        /* The RW stages reload one clock after their issue; a same-edge
         * 6502 or OS write is repaired by the restage that follows it. */
        if (xr_cap0)
            regs[5'h04] <= xr_rdata;
        if (xr_cap1)
            regs[5'h08] <= xr_rdata;
        /* A pop's mirror refill arrives one clock after the pointer
         * moved, always between 6502 cycles; an OS write still outranks
         * it. */
        if (xs_fill)
            regs[5'h0C] <= xs_fill_byte;
        /* Plain shared memory at the system clock: it lands regardless of
         * the 6502's enable, and a same-cell collision goes to the OS. */
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

    initial begin
        txf_wr = 5'd0;
        txf_rd = 5'd0;
        os_rx_valid = 1'b0;
        os_rx_data = 8'h00;
        rx_req = 1'b0;
        xsp = 10'd512;
        xs_fill = 1'b0;
        xs_fill_at = 10'd0;
        xr_wr_pend = 1'b0;
        xr_wr_addr = 16'h0000;
        xr_wr_byte = 8'h00;
        xr_fill_pend0 = 1'b0;
        xr_fill_pend1 = 1'b0;
        xr_bg_alt = 1'b0;
        xr_cap0 = 1'b0;
        xr_cap1 = 1'b0;
    end
    always_ff @(posedge clk) begin
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
        if (en && cs && we && rs == 5'h0F && data_i == 8'h00)
            xsp <= 10'd512; /* DROP */
        /* The xstack bytes land in their own block above; the
         * pointer is the OS's other door. */
        /* A restore re-arms the refill rather than carrying it: $FFEC's
         * mirror is by construction the byte at xsp, and the array and
         * the mirror's word are back before the pointer. A blob cut one
         * clock after a pop would otherwise restore a mirror still
         * holding the byte that pop had already taken. */
        if (b_we && b_word == 8'd200) begin
            xsp <= b_wdata[9:0];
            if (sst_restore) begin
                xs_fill <= 1'b1;
                xs_fill_at <= b_wdata[9:0];
            end
        end
        if (sst_restore && b_we && b_word == 8'(TXQ_PTR)) begin
            txf_wr <= b_wdata[4:0];
            txf_rd <= b_wdata[9:5];
        end
        else begin
            if (push_now)
                txf_wr <= txf_wr + 5'd1;
            if (txf_pop)
                txf_rd <= txf_rd + 5'd1;
        end
        /* A read that tries to stage a byte and finds none sets the
         * request; a byte that lands, whether pulled or offered, clears
         * it. */
        if (en && cs && !we && !pull
            && ((rs == 5'h00 && (regs[0] & RX_READY) == 8'h00)
                || rs == 5'h02))
            rx_req <= 1'b1;
        if (en && pull)
            rx_req <= 1'b0;
        if (sst_restore && b_we && b_word == 8'd18) begin
            rx_req <= b_wdata[1];
            /* Bit 0 rides the window's own polarity -- the OS reads it
             * as "the offer slot is free" -- so the restore undoes the
             * inversion the readback applied. */
            os_rx_valid <= !b_wdata[0];
            os_rx_data <= b_wdata[15:8];
        end else if (b_we && b_word == 8'd18 && b_wdata[9])
            rx_req <= 1'b0;
        else if (b_we && b_word == 8'd18 && !os_rx_valid) begin
            os_rx_valid <= 1'b1;
            os_rx_data <= b_wdata[7:0];
            rx_req <= 1'b0;
        end else if (rx_reclaim || (en && pull && !rx_valid)) begin
            os_rx_valid <= 1'b0;
        end
    end

endmodule
