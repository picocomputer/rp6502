/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The host's filesystem, reached the only way a core can: by asking.
 *
 * Flush is documented by Analogue but absent from its own reference
 * core_bridge_cmd.v, so vendor/openfpga_rp6502 overrides it.
 *
 * The two directions use different memory because the bridge is not
 * symmetric. Inbound already had a path through the SDRAM. Outbound
 * cannot use it: the bridge samples read data four clocks after
 * presenting an address, which is inside the store's turnaround, and
 * Analogue's answer is a prefetch that assumes the host walks addresses
 * in order. Rather than bet a file's contents on that, outbound is a
 * block RAM that always answers in one clock. Open File's 264-byte
 * parameter struct fits the same 512 bytes, and an open always finishes
 * before a write begins.
 *
 * Both this module and the bridge put a deadline on a slot operation,
 * and THE BRIDGE'S MUST EXPIRE FIRST. The bridge holds the resource and
 * only its own retirement frees it. Give up first and the next command
 * walks into a parked bridge: F_ARM proves a command was taken by
 * watching done fall, a parked bridge already has it low, so the
 * command skips the proof, adopts the parked answer as its own, and
 * then executes for real with nobody waiting. This deadline is only the
 * backstop for a bridge that has itself stopped answering.
 */

module pocket_file #(
    /* Where the host finds the outbound buffer on the bridge. The
     * firmware names the same address for Slot Write; mmio.h carries
     * the copy. */
    parameter logic [31:0] WINDOW_BASE = 32'h2000_0000,
    parameter int WINDOW_WORDS = 128,
    /* About 1.8 s at 74.25 MHz — twice the bridge's own deadline, so
     * the bridge always retires into a side still listening. */
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
    output logic [31:0] pocket_file_rd_data,
    output logic [31:0] pocket_file_param_struct,
    output logic [31:0] pocket_file_resp_struct,

    /* The host persists exactly as many bytes as the table names, so a
     * nonvolatile slot holds zero until the machine writes a real
     * size. */
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
    /* Preserved: two flops in series with nothing between them are
     * equivalent, and without a reset to tell them apart the fitter
     * merges them and the crossing loses its synchroniser. */
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
        go_t = 1'b0;
        busy = 1'b0;
        tmo_flag = 1'b0;
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
        /* Write-only: a mux answering for four 32-bit registers is
         * real area on a part with none left. */
        if (stb)
            pocket_file_rdata <= addr[2]
                ? r_result
                : {26'd0, w_pending, tmo_flag, r_err, busy};
    end

    logic [31:0] window[WINDOW_WORDS];
    always_ff @(posedge clk_sys)
        if (win_we)
            window[addr[WA+1:2]] <= wdata;
    /* Taken on the strobe and held until the next one, which is what
     * Analogue's wording allows. Following bridge_addr instead chases a
     * fetch hint for the word after the one the host is still
     * collecting, and hands back the next word every time. */
    always_ff @(posedge clk_74a)
        if (bridge_rd)
            pocket_file_rd_data <= window[bridge_addr[WA+1:2]];

    /* Get File is the reverse — the host writes the name — which the
     * bridge can only do into the store, so it rides a Slot Read's
     * address register. */
    always_comb begin
        pocket_file_param_struct = WINDOW_BASE;
        pocket_file_resp_struct  = pocket_file_bridgeaddr;
    end

    logic [3:0] fstate;
    (* preserve *) logic go_t1, go_t2, go_t3;
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
            pocket_file_flush <= 1'b0;
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
                    pocket_file_flush <= r_op == OP_FLUSH;
                    fstate <= F_ARM;
                end
                /* done is held high between commands, so the fall proves
                 * this one was taken and the rise is the answer. */
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
                /* The loader's read of slot 0 fires off an edge it
                 * cannot be asked to wait on, so this one yields. */
                F_DT0:
                if (!dt_busy) begin
                    pocket_file_dt_req  <= 1'b1;
                    pocket_file_dt_addr <= pocket_file_id[9:0];
                    fstate <= F_DT1;
                end
                /* mf_datatable carries an output register as well as
                 * an address one — outdata_reg_a is CLOCK0 — so the word
                 * lands two clocks after the address, not one. Reading a
                 * clock early returns whatever address was selected
                 * before this one, and that is the loader's, which is
                 * wired to a constant 1: every slot came back holding
                 * slot 0's size. */
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
