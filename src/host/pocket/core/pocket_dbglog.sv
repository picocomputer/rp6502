/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The console again, this time out through the Pocket itself. The debug
 * pin is on the 6515D breakout board; this needs nothing but a Pocket
 * with debug logging switched on, which is the difference between a log
 * anyone can read and a log that needs hardware to read.
 *
 * Target command 0x0152 carries one 32-bit event id, so four console
 * bytes ride in each one, first byte in the top eight bits — the hex the
 * log prints then reads left to right as the text. A partial word is
 * flushed on its own after a short quiet period, because the last line
 * before a hang is the one worth having and it is exactly the one that
 * would otherwise sit in the packer.
 *
 * One event is a round trip through the host, so this is slow, and it
 * drops when the console outruns it. The queue is sized for the boot
 * narration rather than for a running program's output, because no
 * queue that fits is big enough for the second and a log that stalled
 * the machine would report on a machine that no longer exists.
 *
 * The host's own commands ride the same log, because the order it does
 * things in is not documented anywhere and the only way to learn it is
 * to watch. They are snooped off the bridge rather than taken from
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
    /* The machine's clock, which the savestate gate stops. The bytes
     * are the machine's and their valid is a level it drives: on the
     * clock behind the gate, a valid frozen high by a stop would push
     * the same byte on every edge for the whole savestate. */
    input logic clk_mach,
    input logic [7:0] tx_data,
    input logic tx_valid,
    input logic [7:0] rv_tx_data,
    input logic rv_tx_valid,

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

    logic [7:0] byte_in;
    always_comb byte_in = rv_tx_valid ? rv_tx_data : tx_data;

    logic fifo_full, fifo_empty, take;
    logic [7:0] byte_out;

    pocket_fifo #(
        .WIDTH(8),
        .DEPTH_LOG2(7)
    ) q (
        .wclk(clk_mach),
        .w_stb(tx_valid || rv_tx_valid),
        .w_data(byte_in),
        .pocket_fifo_full(fifo_full),
        .rclk(clk_74a),
        .r_take(take),
        .pocket_fifo_empty(fifo_empty),
        .pocket_fifo_rdata(byte_out)
    );

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
    always_comb take = !fifo_empty && state == S_FILL && !emit_cmd;

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
        end else
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

    /* The host register block is decoded the way core_bridge_cmd
     * decodes it, F8xx00xx, so the second byte is not ours to read. */
    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_pocket_dbglog;
    always_comb unused_pocket_dbglog = fifo_full ^ (^bridge_addr[23:16]);
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
