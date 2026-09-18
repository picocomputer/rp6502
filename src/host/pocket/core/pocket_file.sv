/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

module pocket_file #(
    parameter logic [31:0] WINDOW_BASE = 32'h2000_0000,
    parameter int WINDOW_WORDS = 128,
    /* 2^27 clocks is about 1.8 s at 74.25 MHz, twice the 2^26-clock
     * deadline in core_bridge_cmd, so core_bridge_cmd retires a command
     * before this module times it out. If this module timed out first,
     * target_dataslot_done could still be low when the next command
     * entered F_ARM, so F_ARM would pass at once and F_WAIT would
     * capture the abandoned command's late result as the result of the
     * new one. */
    parameter int TIMEOUT_BITS = 27
) (
    input logic clk_sys,
    input logic stb,
    input logic we,
    input logic [27:0] addr,
    input logic [31:0] wdata,
    output logic [31:0] pocket_file_rdata,
    input logic w_pending,

    input logic clk_74a,
    input logic arst_n,
    input logic [31:0] bridge_addr,
    input logic bridge_rd,
    input logic bridge_wr,
    output logic [31:0] pocket_file_rd_data,
    output logic [31:0] pocket_file_param_struct,
    output logic [31:0] pocket_file_resp_struct,

    output logic pocket_file_dt_req,
    output logic [9:0] pocket_file_dt_addr,
    input logic [31:0] datatable_q,
    input logic dt_busy,

    output logic pocket_file_read,
    output logic pocket_file_write,
    output logic pocket_file_openfile,
    output logic pocket_file_getfile,
    output logic pocket_file_flush,
    output logic [15:0] pocket_file_id,
    output logic [31:0] pocket_file_slotoffset,
    output logic [31:0] pocket_file_bridgeaddr,
    output logic [31:0] pocket_file_length,
    input logic target_dataslot_done,
    input logic [2:0] target_dataslot_err
);

    localparam int WA = $clog2(WINDOW_WORDS);

    localparam logic [2:0] OP_READ = 3'd1;
    localparam logic [2:0] OP_WRITE = 3'd2;
    localparam logic [2:0] OP_OPEN = 3'd3;
    localparam logic [2:0] OP_DT = 3'd4;
    localparam logic [2:0] OP_GETFILE = 3'd5;
    localparam logic [2:0] OP_FLUSH = 3'd6;

    localparam logic [3:0] F_IDLE = 4'd0;
    localparam logic [3:0] F_START = 4'd1;
    localparam logic [3:0] F_ARM = 4'd2;
    localparam logic [3:0] F_WAIT = 4'd3;
    localparam logic [3:0] F_DT0 = 4'd4;
    localparam logic [3:0] F_DT1 = 4'd5;
    localparam logic [3:0] F_DT2 = 4'd6;
    localparam logic [3:0] F_DT3 = 4'd7;

    logic [2:0] r_op;
    logic go_t, ret_t, tmo_q;
    logic [2:0] err_q;
    logic [31:0] result_q;

    logic busy, tmo_flag;
    logic [2:0] r_err;
    logic [31:0] r_result;

    logic resp_hit, gf_pend;
    logic wrote_q;
    logic wrote_flag /*verilator public_flat_rd*/;
    (* preserve *) logic ret_t1, ret_t2, ret_t3;
    logic win_we, reg_we;

    always_comb begin
        win_we = stb && we && addr[12];
        reg_we = stb && we && !addr[12];
    end

    initial begin
        pocket_file_id = '0;
        pocket_file_slotoffset = '0;
        pocket_file_length = '0;
        pocket_file_bridgeaddr = '0;
        r_op = '0;
        wrote_flag = 1'b0;
        go_t = 1'b0;
        busy = 1'b0;
        /* tmo_flag starts set because a wake reconfigures the FPGA, and
         * the firmware restored from the savestate may still be polling
         * for a command it issued before the sleep. With busy, error and
         * timeout all clear, that poll would read a command this
         * configuration never ran as a success. fs_start writes
         * FILE_CTL, which clears the flag, whenever the firmware issues
         * a command, so a poll for a command issued after the wake never
         * reads the power-up value. */
        tmo_flag = 1'b1;
        r_err = '0;
        r_result = '0;
        ret_t1 = 1'b0;
        ret_t2 = 1'b0;
        ret_t3 = 1'b0;
        pocket_file_rdata = '0;
    end
    always_ff @(posedge clk_sys) begin
        ret_t1 <= ret_t;
        ret_t2 <= ret_t1;
        ret_t3 <= ret_t2;
        if (ret_t2 != ret_t3) begin
            busy <= 1'b0;
            r_err <= err_q;
            tmo_flag <= tmo_q;
            r_result <= result_q;
            wrote_flag <= wrote_q;
        end
        if (reg_we)
            case (addr[4:2])
                3'd0: pocket_file_id <= wdata[15:0];
                3'd1: pocket_file_slotoffset <= wdata;
                3'd2: pocket_file_length <= wdata;
                3'd3: pocket_file_bridgeaddr <= wdata;
                3'd4: begin
                    r_op <= wdata[2:0];
                    go_t <= !go_t;
                    busy <= 1'b1;
                    tmo_flag <= 1'b0;
                    wrote_flag <= 1'b0;
                end
                default: ;
            endcase
        if (stb)
            pocket_file_rdata <= addr[2]
                ? r_result
                : {25'd0, wrote_flag, w_pending, tmo_flag, r_err, busy};
    end

    logic [31:0] window[WINDOW_WORDS];
    always_ff @(posedge clk_sys)
        if (win_we)
            window[addr[WA+1:2]] <= wdata;
    /* io_bridge_peripheral.v sends the host the value on bridge_rd_data
     * and only then strobes bridge_rd for the new address, and its
     * header states that reads are buffered by one word, so each read
     * returns the word addressed by the read before it. Loading on
     * bridge_rd holds that word until the next read, and reading
     * window[bridge_addr] directly would return the word meant for the
     * next read instead. */
    always_ff @(posedge clk_74a)
        if (bridge_rd)
            pocket_file_rd_data <= window[bridge_addr[WA+1:2]];

    always_comb begin
        pocket_file_param_struct = WINDOW_BASE;
        pocket_file_resp_struct  = pocket_file_bridgeaddr;
    end

    /* The host acknowledges a command by writing 'bu' and 'ok' to
     * target_0 at 0xF8xx1000, so resp_hit matches only bridge writes to
     * the staging store at 0x0xxxxxxx. */
    always_comb resp_hit = bridge_wr && gf_pend
        && bridge_addr[31:28] == 4'h0;

    logic [3:0] fstate;
    (* preserve *) logic go_t1, go_t2, go_t3;
    logic [TIMEOUT_BITS-1:0] tmo;

    always_ff @(posedge clk_74a or negedge arst_n) begin
        if (!arst_n) begin
            fstate <= F_IDLE;
            gf_pend <= 1'b0;
            wrote_q <= 1'b0;
            go_t1 <= 1'b0;
            go_t2 <= 1'b0;
            go_t3 <= 1'b0;
            ret_t <= 1'b0;
            tmo_q <= 1'b0;
            err_q <= '0;
            result_q <= '0;
            tmo <= '0;
            pocket_file_read <= 1'b0;
            pocket_file_write <= 1'b0;
            pocket_file_openfile <= 1'b0;
            pocket_file_getfile <= 1'b0;
            pocket_file_flush <= 1'b0;
            pocket_file_dt_req <= 1'b0;
            pocket_file_dt_addr <= '0;
        end else begin
            go_t1 <= go_t;
            go_t2 <= go_t1;
            go_t3 <= go_t2;
            tmo   <= tmo + 1'b1;
            if (resp_hit)
                wrote_q <= 1'b1;
            if (fstate == F_IDLE)
                gf_pend <= 1'b0;
            case (fstate)
                F_START: begin
                    wrote_q <= 1'b0;
                    pocket_file_read <= r_op == OP_READ;
                    pocket_file_write <= r_op == OP_WRITE;
                    pocket_file_openfile <= r_op == OP_OPEN;
                    pocket_file_getfile <= r_op == OP_GETFILE;
                    pocket_file_flush <= r_op == OP_FLUSH;
                    gf_pend <= r_op == OP_GETFILE;
                    fstate <= F_ARM;
                end
                /* target_dataslot_done rises when a data slot command
                 * finishes or times out and stays high until
                 * core_bridge_cmd dispatches the next data slot command,
                 * so F_ARM waits for it to fall and F_WAIT waits for it
                 * to rise. */
                F_ARM: begin
                    pocket_file_read <= 1'b0;
                    pocket_file_write <= 1'b0;
                    pocket_file_openfile <= 1'b0;
                    pocket_file_getfile <= 1'b0;
                    pocket_file_flush <= 1'b0;
                    if (!target_dataslot_done)
                        fstate <= F_WAIT;
                    else if (&tmo) begin
                        tmo_q  <= 1'b1;
                        ret_t  <= !ret_t;
                        fstate <= F_IDLE;
                    end
                end
                F_WAIT:
                if (target_dataslot_done) begin
                    err_q  <= target_dataslot_err;
                    ret_t  <= !ret_t;
                    fstate <= F_IDLE;
                end else if (&tmo) begin
                    tmo_q  <= 1'b1;
                    ret_t  <= !ret_t;
                    fstate <= F_IDLE;
                end
                F_DT0:
                if (!dt_busy) begin
                    pocket_file_dt_req  <= 1'b1;
                    pocket_file_dt_addr <= pocket_file_id[9:0];
                    fstate <= F_DT1;
                end
                /* mf_datatable registers both the address and the
                 * output of port A (outdata_reg_a is CLOCK0), so the word
                 * is on datatable_q two clocks after the address is
                 * presented in F_DT1, and F_DT3 samples it. */
                F_DT1: fstate <= dt_busy ? F_DT0 : F_DT2;
                F_DT2: fstate <= dt_busy ? F_DT0 : F_DT3;
                F_DT3:
                if (dt_busy)
                    fstate <= F_DT0;
                else begin
                    result_q <= datatable_q;
                    pocket_file_dt_req <= 1'b0;
                    ret_t  <= !ret_t;
                    fstate <= F_IDLE;
                end
                default:
                if (go_t2 != go_t3) begin
                    err_q  <= '0;
                    tmo_q  <= 1'b0;
                    tmo    <= '0;
                    fstate <= r_op == OP_DT ? F_DT0 : F_START;
                end
            endcase
        end
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_pocket_file;
    always_comb unused_pocket_file = ^{addr[27:13], addr[11:5], addr[1:0],
                                       bridge_addr[31:WA+2],
                                       bridge_addr[1:0],
                                       pocket_file_id[15:10]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
