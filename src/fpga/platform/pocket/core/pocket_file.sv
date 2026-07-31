/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The host's filesystem, reached the only way a core can reach it: by
 * asking. Four target commands move a data slot's file — Slot Read
 * copies from it into memory the bridge can write, Slot Write copies
 * out of memory the bridge can read, Open File hands the host a name
 * and binds it to a slot, Get File asks which name a slot already
 * holds — and the data table says how long the file is. This block
 * issues all five for the soft CPU and answers when they retire. Get
 * File has no caller yet; it is wired because a fit is half an hour
 * and a missing wire is not worth one.
 *
 * The two directions use different memory, because the bridge is not
 * symmetric. Writes toward the core already had a path — the loader
 * streams data slots into the SDRAM and has since the first ROM — so
 * Slot Read simply names an SDRAM address and the machine reads the
 * bytes back through its staging window. Reads out of the core had
 * none, and cannot use the SDRAM: the bridge samples read data four
 * clocks after it presents an address, which is less than the store's
 * turnaround, and Analogue's own answer to that is a prefetch that
 * assumes the host walks addresses in order. Rather than bet a file's
 * contents on an assumption, the outbound side is a block RAM that
 * always answers in one clock. It is small — 512 bytes, one M10K in
 * the shape one write port and one read port take — and Open File's
 * 264-byte parameter struct fits inside it, so the same buffer carries
 * the filename going out and the file data going out. Never at once:
 * an open is finished before a write begins.
 *
 * Every command is one crossing. The machine leaves its parameters
 * standing, flips a toggle, and waits; the parameters are quasi-static
 * behind that toggle, with three flops of synchroniser passing before
 * the bridge side reads them, and the answer returns the same way.
 * core_bridge_cmd puts no deadline on a data slot operation, unlike
 * the one it puts on a debug event, so the deadline is here: a host
 * that never answers retires the command as a timeout rather than
 * leaving the machine waiting on a file forever.
 */

module pocket_file #(
    /* Where the host finds the outbound buffer on the bridge. The
     * firmware names the same address for Slot Write; mmio.h carries
     * the copy. */
    parameter logic [31:0] WINDOW_BASE = 32'h2000_0000,
    parameter int WINDOW_WORDS = 128,
    /* About 1.8 s at 74.25 MHz. */
    parameter int TIMEOUT_BITS = 27
) (
    /* The machine's side: the soft CPU's platform window. */
    input logic clk_sys,
    input logic rst_n,
    input logic stb,
    input logic we,
    input logic [27:0] addr,
    input logic [31:0] wdata,
    output logic [31:0] pocket_file_rdata,
    /* Halfwords the host wrote that the store has not taken yet. */
    input logic w_pending,

    /* The bridge's side. */
    input logic clk_74a,
    input logic arst_n,
    input logic [31:0] bridge_addr,
    input logic bridge_rd,
    output logic [31:0] pocket_file_rd_data,
    output logic [31:0] pocket_file_param_struct,
    output logic [31:0] pocket_file_resp_struct,

    /* The data table, shared with the loader's own read of slot 0. */
    output logic pocket_file_dt_req,
    output logic [9:0] pocket_file_dt_addr,
    input logic [31:0] datatable_q,
    input logic dt_busy,

    /* core_bridge_cmd's target commands. */
    output logic pocket_file_read,
    output logic pocket_file_write,
    output logic pocket_file_openfile,
    output logic pocket_file_getfile,
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

    localparam logic [2:0] F_IDLE = 3'd0;
    localparam logic [2:0] F_START = 3'd1;
    localparam logic [2:0] F_ARM = 3'd2;
    localparam logic [2:0] F_WAIT = 3'd3;
    localparam logic [2:0] F_DT0 = 3'd4;
    localparam logic [2:0] F_DT1 = 3'd5;
    localparam logic [2:0] F_DT2 = 3'd6;

    /* The command, and the answer, standing across the crossing. */
    logic [2:0] r_op;
    logic go_t, ret_t, tmo_q;
    logic [2:0] err_q;
    logic [31:0] result_q;

    /* --- The machine's registers, clk_sys side. --- */
    logic busy, tmo_flag;
    logic [2:0] r_err;
    logic [31:0] r_result;
    logic ret_t1, ret_t2, ret_t3;
    logic win_we, reg_we;

    always_comb begin
        win_we = stb && we && addr[12];
        reg_we = stb && we && !addr[12];
    end

    always_ff @(posedge clk_sys or negedge rst_n) begin
        if (!rst_n) begin
            pocket_file_id <= '0;
            pocket_file_slotoffset <= '0;
            pocket_file_length <= '0;
            pocket_file_bridgeaddr <= '0;
            r_op <= '0;
            go_t <= 1'b0;
            busy <= 1'b0;
            tmo_flag <= 1'b0;
            r_err <= '0;
            r_result <= '0;
            ret_t1 <= 1'b0;
            ret_t2 <= 1'b0;
            ret_t3 <= 1'b0;
            pocket_file_rdata <= '0;
        end else begin
            ret_t1 <= ret_t;
            ret_t2 <= ret_t1;
            ret_t3 <= ret_t2;
            if (ret_t2 != ret_t3) begin
                busy <= 1'b0;
                r_err <= err_q;
                tmo_flag <= tmo_q;
                r_result <= result_q;
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
                    end
                    default: ;
                endcase
            /* The parameters are write-only. Nothing reads back what it
             * just wrote, and a mux that answers for four 32-bit
             * registers is real area on a part with none left. */
            if (stb)
                pocket_file_rdata <= addr[2]
                    ? r_result
                    : {26'd0, w_pending, tmo_flag, r_err, busy};
        end
    end

    /* --- The outbound buffer: written by the machine, read by the
     * host. One write port, one read port, a clock each. --- */
    logic [31:0] window[WINDOW_WORDS];
    always_ff @(posedge clk_sys)
        if (win_we)
            window[addr[WA+1:2]] <= wdata;
    /* The read is taken on the strobe and held until the next one:
     * "the core may not immediately provide the read data and has up
     * until the next read strobe to drive bridge_rd_data". Following
     * bridge_addr instead means moving the answer on the moment the
     * host moves the address, and the host moves it early — it is a
     * fetch hint for the word after the one it is still collecting. A
     * core that chases it hands back the next word every time, which is
     * how T2.DAT went out and [AT] came back. */
    always_ff @(posedge clk_74a)
        if (bridge_rd)
            pocket_file_rd_data <= window[bridge_addr[WA+1:2]];

    /* Open File's parameter struct is the one thing the host reads out
     * of the window, so its pointer is the window. Get File's response
     * struct is the reverse — the host writes the name — which the
     * bridge can only do into the store, so it rides the same address
     * register a Slot Read does. */
    always_comb begin
        pocket_file_param_struct = WINDOW_BASE;
        pocket_file_resp_struct  = pocket_file_bridgeaddr;
    end

    /* --- The command, clk_74a side. --- */
    logic [2:0] fstate;
    logic go_t1, go_t2, go_t3;
    logic [TIMEOUT_BITS-1:0] tmo;

    always_ff @(posedge clk_74a or negedge arst_n) begin
        if (!arst_n) begin
            fstate <= F_IDLE;
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
            pocket_file_dt_req <= 1'b0;
            pocket_file_dt_addr <= '0;
        end else begin
            go_t1 <= go_t;
            go_t2 <= go_t1;
            go_t3 <= go_t2;
            tmo   <= tmo + 1'b1;
            case (fstate)
                F_START: begin
                    pocket_file_read <= r_op == OP_READ;
                    pocket_file_write <= r_op == OP_WRITE;
                    pocket_file_openfile <= r_op == OP_OPEN;
                    pocket_file_getfile <= r_op == OP_GETFILE;
                    fstate <= F_ARM;
                end
                /* The bridge holds done high from one command until the
                 * next is issued, so the fall is what proves this
                 * command was taken and the rise after it is the
                 * answer. pocket_dbglog waits on the same two edges. */
                F_ARM: begin
                    pocket_file_read <= 1'b0;
                    pocket_file_write <= 1'b0;
                    pocket_file_openfile <= 1'b0;
                    pocket_file_getfile <= 1'b0;
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
                /* The loader's own read of slot 0 fires off an edge it
                 * cannot be asked to wait on, so this one yields and
                 * starts over. The machine is polling either way. */
                F_DT0:
                if (!dt_busy) begin
                    pocket_file_dt_req  <= 1'b1;
                    pocket_file_dt_addr <= pocket_file_id[9:0];
                    fstate <= F_DT1;
                end
                F_DT1: fstate <= dt_busy ? F_DT0 : F_DT2;
                F_DT2:
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
