/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The console, out through the Pocket's own debug log.
 *
 * Target command 0x0152 carries one 32-bit event id, so four console
 * bytes ride in each one, first byte in the top eight bits — the hex the
 * log prints then reads left to right as the text. A partial word is
 * flushed after a quiet period, because the last line before a hang is
 * the one worth having and it is exactly the one that would otherwise
 * sit in the packer.
 *
 * An event is a whole host round trip, so this drains orders of
 * magnitude slower than a firmware that is printing. There is no queue,
 * because no queue that fits is big enough and one that does not fit
 * shreds a report instead of truncating it — bytes from two lines
 * interleaved, which reads as a line that was never printed. The
 * writer waits instead: pocket_dbglog_full is the handoff saying the
 * last byte has not been packed yet, and the console this carries is
 * debug only.
 *
 * Host commands are snooped off the bridge rather than taken from
 * core_bridge_cmd: the write that carries a command is broadcast to
 * every device already, so the vendor file stays as it is.
 *
 * A command event is 0xC0 in the top byte, which no console byte can
 * be — the console is bytes and this is a word, and a word whose top
 * byte is 0xC0 would need four console bytes to line up on the packer's
 * own boundary to forge one. Commands in the 0x008x and 0x00Ax
 * families put their parameter word out as a second event immediately
 * after, unprefixed, because a parameter can be any 32 bits at all; it
 * is read as the payload of the 0xC0 before it.
 *
 * Commands go out between console words rather than through the packer,
 * so a command never splits a line, and a line already half-packed is
 * flushed early to let one past instead of holding it for the quiet
 * period. Ordering is worth more than tidiness here.
 */

module pocket_dbglog #(
    /* About 0.9 ms of quiet at 74.25 MHz before a short word goes. */
    parameter int FLUSH_TICKS = 65536
) (
    /* The machine's clock, which the savestate gate stops. The valid is
     * a level the machine drives: on the clock behind the gate, one
     * frozen high by a stop would push the same byte on every edge for
     * the whole savestate.
     *
     * One console. The 6502's $FFE1 reaches it the same way everything
     * else does — com_task drains that queue and prints what it finds —
     * so taking $FFE1 here as well would log every program byte twice
     * and put two writers on a channel that carries one at a time. */
    input logic clk_mach,
    input logic [7:0] tx_data,
    input logic tx_valid,
    output logic pocket_dbglog_full,

    input logic clk_74a,
    input logic arst_n,
    input logic bridge_wr,
    input logic bridge_endian_little,
    input logic [31:0] bridge_addr,
    input logic [31:0] bridge_wr_data,
    input logic target_debug_done,
    output logic pocket_dbglog_event,
    output logic [31:0] pocket_dbglog_id
);

    /* The crossing is the storage: one byte, a toggle each way. Held
     * still for the whole of its round trip, so the far side reads it
     * whenever it gets to it and the near side knows when it may write
     * the next.
     *
     * Preserved: two flops in series with nothing between them are
     * equivalent, and without a reset to tell them apart the fitter
     * merges them and the crossing loses its synchroniser. */
    logic req_t, ack_t, busy;
    logic [7:0] hand;
    (* preserve *) logic ack_t1, ack_t2;
    always_comb begin
        busy = req_t != ack_t2;
        pocket_dbglog_full = busy;
    end

    initial begin
        req_t  = 1'b0;
        hand   = 8'h00;
        ack_t1 = 1'b0;
        ack_t2 = 1'b0;
    end
    always_ff @(posedge clk_mach) begin
        ack_t1 <= ack_t;
        ack_t2 <= ack_t1;
        if (!busy && tx_valid) begin
            hand  <= tx_data;
            req_t <= !req_t;
        end
    end

    logic take, have;
    logic [7:0] byte_out;
    (* preserve *) logic req_t1, req_t2, req_t3;
    always_comb byte_out = hand;

    /* The command register is F8xx0000 and the first parameter register
     * F8xx0020, the same decode core_bridge_cmd uses, and "CM" in the
     * top half is what makes a write a command rather than a status
     * readback. The parameter is written before the command that
     * consumes it, so latching it and emitting it afterwards puts the
     * pair in the log in the order they were meant. */
    logic [31:0] wr_data;
    always_comb
        wr_data = bridge_endian_little
            ? {bridge_wr_data[7:0], bridge_wr_data[15:8],
               bridge_wr_data[23:16], bridge_wr_data[31:24]}
            : bridge_wr_data;

    logic host_reg, cmd_hit, param_hit;
    always_comb begin
        host_reg = bridge_wr && bridge_addr[31:24] == 8'hF8
            && bridge_addr[15:8] == 8'h00;
        cmd_hit = host_reg && bridge_addr[7:0] == 8'h00
            && wr_data[31:16] == 16'h434D;
        param_hit = host_reg && bridge_addr[7:0] == 8'h20;
    end

    /* The five commands that carry a parameter worth reading: which
     * slot, how big, and whether a savestate word is a query or an ask.
     * Named rather than taken as a range, because the family they are
     * in also holds 0x008F, which has no parameters at all and would
     * otherwise put the previous command's word out as its own. */
    logic param_worth;
    always_comb
        param_worth = wr_data[15:0] == 16'h0080 || wr_data[15:0] == 16'h0082
            || wr_data[15:0] == 16'h008A || wr_data[15:0] == 16'h00A0
            || wr_data[15:0] == 16'h00A4;

    localparam int CQ_LOG2 = 3;
    /* Eight words. Left to itself the fitter gives 256 bits a whole
     * block, which is one of twenty this design has left. */
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [31:0] cq[1 << CQ_LOG2];
    logic [CQ_LOG2-1:0] cq_w, cq_r;
    logic cq_empty, cq_full, cq_push;
    logic [31:0] cq_wdata;
    logic [31:0] host_param;
    logic param_due;
    always_comb begin
        cq_empty = cq_w == cq_r;
        cq_full = (CQ_LOG2)'(cq_w + 1'b1) == cq_r;
        cq_push = cmd_hit || param_due;
        cq_wdata = cmd_hit ? {8'hC0, 8'h00, wr_data[15:0]} : host_param;
    end

    /* The parameter goes out the cycle after its command, which is safe
     * because the host cannot start a command while one is running —
     * core_bridge_cmd queues nothing and says so. Two command writes on
     * neighbouring bridge cycles would cost the first its parameter,
     * and that is a host doing something it cannot do. */
    always_ff @(posedge clk_74a or negedge arst_n) begin
        if (!arst_n) begin
            cq_w <= '0;
            host_param <= '0;
            param_due <= 1'b0;
        end else begin
            if (param_hit) host_param <= wr_data;
            if (cmd_hit) param_due <= param_worth;
            else if (param_due) param_due <= 1'b0;
            if (cq_push && !cq_full) begin
                cq[cq_w] <= cq_wdata;
                cq_w <= (CQ_LOG2)'(cq_w + 1'b1);
            end
        end
    end

    /* The bridge holds its done high from one command until the next is
     * issued, so an edge is the only thing worth believing: arm, watch
     * it fall, then watch it rise. It always rises — the bridge times
     * out an unanswered command itself — so there is nothing to give up
     * on here, and a second deadline racing that one would only let go
     * of a word the bridge is still about to send. */
    localparam logic [1:0] S_FILL = 2'd0;
    localparam logic [1:0] S_ARM = 2'd1;
    localparam logic [1:0] S_WAIT = 2'd2;

    logic [1:0] state;
    logic [1:0] count;
    logic [$clog2(FLUSH_TICKS)-1:0] quiet;

    /* A command goes out at a word boundary, ahead of the console, so
     * the packer never has to be interrupted mid-word. */
    logic emit_cmd;
    always_comb emit_cmd = state == S_FILL && count == 2'd0 && !cq_empty;
    always_comb take = have && state == S_FILL && !emit_cmd;

    /* Two's complement in two bits is 4 - count for the one, two and
     * three byte cases, which is the shift that left-justifies them. */
    logic [1:0] pad;
    always_comb pad = 2'd0 - count;

    always_ff @(posedge clk_74a or negedge arst_n) begin
        if (!arst_n) begin
            pocket_dbglog_event <= 1'b0;
            pocket_dbglog_id <= '0;
            state <= S_FILL;
            count <= '0;
            quiet <= '0;
            cq_r <= '0;
            req_t1 <= 1'b0;
            req_t2 <= 1'b0;
            req_t3 <= 1'b0;
            have <= 1'b0;
            ack_t <= 1'b0;
        end else begin
            req_t1 <= req_t;
            req_t2 <= req_t1;
            req_t3 <= req_t2;
            /* Consumed and answered in the same edge it is packed on.
             * The far side cannot present another until this ack has
             * crossed back, so the two never land together. */
            if (take) begin
                have  <= 1'b0;
                ack_t <= !ack_t;
            end
            if (req_t2 != req_t3) have <= 1'b1;
            case (state)
                S_ARM: if (!target_debug_done) state <= S_WAIT;
                S_WAIT:
                if (target_debug_done) begin
                    pocket_dbglog_event <= 1'b0;
                    state <= S_FILL;
                end
                default:
                if (emit_cmd) begin
                    pocket_dbglog_id <= cq[cq_r];
                    cq_r <= (CQ_LOG2)'(cq_r + 1'b1);
                    pocket_dbglog_event <= 1'b1;
                    state <= S_ARM;
                end else if (take) begin
                    pocket_dbglog_id <= {pocket_dbglog_id[23:0], byte_out};
                    quiet <= '0;
                    if (count == 2'd3) begin
                        count <= '0;
                        pocket_dbglog_event <= 1'b1;
                        state <= S_ARM;
                    end else count <= count + 2'd1;
                end else if (count != 2'd0) begin
                    /* A command waiting behind a half-packed line is
                     * worth more than the line's last byte or two
                     * arriving whole, so it does not wait out the
                     * quiet period. */
                    if (!cq_empty
                        || quiet == ($clog2(FLUSH_TICKS))'(FLUSH_TICKS - 1))
                    begin
                        pocket_dbglog_id <= pocket_dbglog_id << {pad, 3'b000};
                        count <= '0;
                        quiet <= '0;
                        pocket_dbglog_event <= 1'b1;
                        state <= S_ARM;
                    end else quiet <= quiet + 1'b1;
                end
            endcase
        end
    end

    /* The host register block is decoded the way core_bridge_cmd
     * decodes it, F8xx00xx, so the second byte is not ours to read. */
    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_pocket_dbglog;
    always_comb unused_pocket_dbglog = ^bridge_addr[23:16];
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
