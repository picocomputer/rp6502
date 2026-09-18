/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

module pocket_dbglog #(
    /* 65536 clocks is about 0.9 ms at 74.25 MHz, which is how long a
     * partial word is held for another byte while no host command is
     * queued. */
    parameter int FLUSH_TICKS = 65536
) (
    input logic clk_mach,
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

    logic fifo_full, fifo_empty, take;
    logic [7:0] byte_out;

    pocket_fifo #(
        .WIDTH(8),
        .DEPTH_LOG2(7)
    ) q (
        .wclk(clk_mach),
        .w_stb(rv_tx_valid),
        .w_data(rv_tx_data),
        .pocket_fifo_full(fifo_full),
        .rclk(clk_74a),
        .r_take(take),
        .pocket_fifo_empty(fifo_empty),
        .pocket_fifo_rdata(byte_out)
    );

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

    logic param_worth;
    always_comb
        param_worth = wr_data[15:0] == 16'h0080 || wr_data[15:0] == 16'h0082
            || wr_data[15:0] == 16'h008A || wr_data[15:0] == 16'h00A0
            || wr_data[15:0] == 16'h00A4;

    localparam int CQ_LOG2 = 3;
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

    /* The host writes a command's parameter to F8xx0020 before it
     * writes the command, so host_param already holds the parameter when
     * cmd_hit fires, and for a command that param_worth selects, the
     * parameter is pushed on the next clock. A second command write on
     * that clock would drop the parameter, but the host does not start a
     * command until the previous one has finished, as core_bridge_cmd
     * notes at ST_IDLE. */
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

    /* target_debug_done rises when a debug event, which is target command
     * 0x0152, finishes or times out, and it stays high until
     * core_bridge_cmd dispatches the next 0x0152 command, so S_ARM waits
     * for it to fall and S_WAIT waits for it to rise. */
    localparam logic [1:0] S_FILL = 2'd0;
    localparam logic [1:0] S_ARM = 2'd1;
    localparam logic [1:0] S_WAIT = 2'd2;

    logic [1:0] state;
    logic [1:0] count;
    logic [$clog2(FLUSH_TICKS)-1:0] quiet;

    logic emit_cmd;
    always_comb emit_cmd = state == S_FILL && count == 2'd0 && !cq_empty;
    always_comb take = !fifo_empty && state == S_FILL && !emit_cmd;

    /* In two bits, 0 - count is 4 - count for a partial word of one to
     * three bytes, and shifting by that many bytes left-justifies the
     * word. */
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

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_pocket_dbglog;
    always_comb unused_pocket_dbglog = fifo_full ^ (^bridge_addr[23:16]);
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
