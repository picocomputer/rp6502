/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The PCM: device's sound, bit-exact with core/aud/pcm.c built with
 * AUD_NATIVE_RATE 48000: pcm_sample on every tick.
 *
 * The soft CPU writes frames into a 4096-byte FIFO, reading the level
 * first so that it never overruns it. A tick whose phase reaches one
 * takes a frame, or silence when none is whole, into a history of the
 * last 24 per channel, and every tick filters it with two rows of
 * rsmp_rom's coefficients and blends them as pcm_blend does, in about
 * 105 of the tick's 1050 clocks.
 *
 * Everything that reads the FIFO and the phase happens on the tick's
 * edge, so a write, clear, open or close lands before a sample or after
 * it. One on the tick's own clock is after it.
 *
 * A savestate reads and writes the FIFO through the sst port at savestate
 * offset 0, a row of four bytes a word, the history at 0x600 and the
 * flops at 0x790. Its word comes in on wdata. Between samples every other
 * flop and every registered read is dead, so only that state is kept.
 */

module pcm
    import rsmp_coef_pkg::*;
(
    input logic clk,
    input logic clk_sys,
    input logic tick,

    /* Bit 0 open, bit 1 16-bit, bit 2 stereo. Opening empties the FIFO
     * and resets the player at the next tick. */
    input logic ctrl_we,
    /* Frames per sample, Q32, less one. Written while closed. */
    input logic step_we,
    /* Four bytes, wdata[7:0] first. */
    input logic data_we,
    /* wdata[7:0]. */
    input logic byte_we,
    /* Empties the FIFO; the history plays out. */
    input logic clear_we,
    /* The register's word, and the savestate's on sst_we or sst_st_we. */
    input logic [31:0] wdata,

    /* rsmp_rom's port, read a clock after the address. */
    output logic [7:0] pcm_coef_row,
    output logic [4:0] pcm_coef_tap,
    input logic signed [RSMP_COEF_W-1:0] coef_q,

    /* Bytes waiting to play, 0..4096. */
    output logic [12:0] pcm_level,
    /* Held from the end of a sample's filter to the next tick's. */
    output logic signed [RSMP_OUT_W-1:0] pcm_l,
    output logic signed [RSMP_OUT_W-1:0] pcm_r,

    /* sst_own gives the FIFO's and the history's ports to sst_addr,
     * written on sst_we. sst_st_we writes the flop word at sst_addr on
     * clk. */
    input logic sst_own,
    input logic [10:0] sst_addr,
    input logic sst_we,
    input logic sst_st_we,
    output logic [31:0] pcm_sst_rdata,
    output logic pcm_quiet
);

    logic sel_f, sel_h, sel_st;
    always_comb begin
        sel_f = !sst_addr[10];
        sel_h = sst_addr[10:6] == 5'h18;
        sel_st = sst_addr[10:3] == 8'hF2;
    end

    localparam logic [32:0] ONE = 33'h1_0000_0000;
    localparam int DH_W = RSMP_DIFF_W - 16;

    logic open, b16, st;
    logic [31:0] step_m1;
    logic rst_pend;

    typedef enum logic [2:0] {
        S_IDLE, S_PUSH, S_MAC, S_DRAIN, S_SCALE, S_SUM
    } state_t;
    state_t state /*verilator public_flat_rd*/;

    logic [32:0] phase, ph;
    always_comb ph = rst_pend ? ONE : phase;
    logic go;
    always_comb go = tick && open && state == S_IDLE;

    /* FIFO byte i is bank i & 3, row i >> 2, so any four bytes in a row
     * are one per bank. The savestate's row takes byte b of its word into
     * bank b through the same lanes. */
    logic [12:0] head, tail;
    always_comb pcm_level = head - tail;
    logic [2:0] frame;
    always_comb frame = 3'd1 << (2'(b16) + 2'(st));
    logic has;
    always_comb has = pcm_level >= 13'(frame);

    logic [3:0] w_en;
    logic [9:0] w_row[4], r_row[4];
    logic [7:0] w_byte[4];
    logic [1:0] w_k[4];
    always_comb
        for (int b = 0; b < 4; b++) begin
            w_k[b] = sst_own ? 2'(b) : 2'(b) - head[1:0];
            w_en[b] = sst_own ? sst_we && sel_f
                : data_we || (byte_we && w_k[b] == 2'd0);
            w_row[b] = sst_own ? sst_addr[9:0]
                : head[11:2] + 10'(2'(b) < head[1:0]);
            w_byte[b] = wdata[{w_k[b], 3'd0}+:8];
            r_row[b] = tail[11:2] + 10'(2'(b) < tail[1:0]);
        end

    logic [7:0] fq[4], fa_q[4];
    genvar gb;
    generate
        for (gb = 0; gb < 4; gb++) begin : gen_bank
            pcm_bank bank (
                .clk_a(clk_sys),
                .clk(clk),
                .we(w_en[gb]),
                .waddr(w_row[gb]),
                .wdata(w_byte[gb]),
                .pcm_bank_a_q(fa_q[gb]),
                .re(tick),
                .raddr(r_row[gb]),
                .pcm_bank_q(fq[gb])
            );
        end
    endgenerate

    /* The take's frame: whether it was whole, its format and where in
     * the four banks it starts. */
    logic tk_has, tk_16, tk_st;
    logic [1:0] tk_t;

    logic ch, pass;
    logic [4:0] tap;
    /* Where the next push goes, and the window's taps below cold read
     * zero: the pushes since a reset are 24 - cold. */
    logic [4:0] at, cold;

    logic [1:0] d_o;
    logic [7:0] d_lo, d_hi;
    logic signed [15:0] dec;
    always_comb begin
        d_o = tk_t + (ch && tk_st ? (tk_16 ? 2'd2 : 2'd1) : 2'd0);
        d_lo = fq[d_o];
        d_hi = fq[d_o + 2'd1];
        if (!tk_has)
            dec = '0;
        else if (tk_16)
            dec = {d_hi, d_lo};
        else
            dec = {~d_lo[7], d_lo[6:0], 8'd0};
    end

    (* ramstyle = "MLAB, no_rw_check" *)
    logic signed [15:0] hist[64];
    logic [5:0] hw_a;
    always_comb hw_a = sst_own ? sst_addr[5:0] : {ch, at};
    always_ff @(posedge clk_sys)
        if (sst_own ? sst_we && sel_h : state == S_PUSH)
            hist[hw_a] <= sst_own ? wdata[15:0] : dec;
    logic [5:0] h_ra, hr_a;
    logic signed [15:0] h_rd, hist_q;
    always_comb begin
        h_ra = {ch, at + 5'(32 - RSMP_TAPS) + tap};
        hr_a = sst_own ? sst_addr[5:0] : h_ra;
        h_rd = hist[hr_a];
    end

    always_comb begin
        pcm_coef_row = 8'(phase[31:25]) + 8'(pass);
        pcm_coef_tap = tap;
    end

    /* Row p is subtracted and row p + 1 added: acc ends at d, their
     * difference, and acc_a holds row p's dot negated. */
    logic mac_en, mac_add, mac_cap;
    logic signed [RSMP_DOT_W-1:0] acc, acc_a, prod;
    always_comb prod = RSMP_DOT_W'(coef_q) * RSMP_DOT_W'(hist_q);

    /* s = (d * f) >> 16 = dh * f + ((dl * f) >> 16), two products that fit
     * an 18x19 multiplier each, plus the rounding half. */
    logic signed [RSMP_DIFF_W-1:0] s_q;
    logic signed [DH_W-1:0] dh;
    logic [15:0] dl, f;
    logic signed [RSMP_DIFF_W-1:0] s_hi;
    logic [15:0] s_lo;
    always_comb begin
        {dh, dl} = acc[RSMP_DIFF_W-1:0];
        f = phase[24:9];
        s_hi = RSMP_DIFF_W'(dh) * RSMP_DIFF_W'($signed({1'b0, f}));
        s_lo = 16'((32'(dl) * 32'(f)) >> 16);
    end

    logic signed [RSMP_DOT_W-1:0] v;
    always_comb v = RSMP_DOT_W'(s_q) - acc_a;

    initial begin
        open = 1'b0;
        b16 = 1'b0;
        st = 1'b0;
        step_m1 = '0;
        rst_pend = 1'b0;
        state = S_IDLE;
        phase = '0;
        head = '0;
        tail = '0;
        tk_has = 1'b0;
        tk_16 = 1'b0;
        tk_st = 1'b0;
        tk_t = '0;
        ch = 1'b0;
        pass = 1'b0;
        tap = '0;
        at = '0;
        cold = '0;
        mac_en = 1'b0;
        mac_add = 1'b0;
        mac_cap = 1'b0;
        acc = '0;
        acc_a = '0;
        hist_q = '0;
        s_q = '0;
        pcm_l = '0;
        pcm_r = '0;
    end

    always_ff @(posedge clk) begin
        if (ctrl_we)
            {st, b16, open} <= wdata[2:0];
        if (step_we)
            step_m1 <= wdata;
        if (ctrl_we && wdata[0])
            rst_pend <= 1'b1;
        else if (go)
            rst_pend <= 1'b0;

        if (data_we)
            head <= head + 13'd4;
        else if (byte_we)
            head <= head + 13'd1;
        if (go && ph[32] && has)
            tail <= tail + 13'(frame);
        if (clear_we)
            tail <= head;
        if (ctrl_we && wdata[0]) begin
            head <= '0;
            tail <= '0;
        end
        if (sst_st_we && sel_st)
            case (sst_addr[2:0])
                3'd0: step_m1 <= wdata;
                3'd2: {rst_pend, head, tail, st, b16, open} <= wdata[29:0];
                default: ;
            endcase
    end

    always_ff @(posedge clk) begin
        hist_q <= tap < cold ? '0 : h_rd;
        mac_en <= 1'b0;
        mac_cap <= 1'b0;
        if (mac_en)
            acc <= mac_add ? acc + prod : acc - prod;
        if (mac_cap)
            acc_a <= acc;

        case (state)
            S_IDLE:
                if (tick && !open) begin
                    pcm_l <= '0;
                    pcm_r <= '0;
                end else if (go) begin
                    phase <= {1'b0, ph[31:0]};
                    if (rst_pend)
                        cold <= 5'(RSMP_TAPS);
                    tk_has <= has;
                    tk_16 <= b16;
                    tk_st <= st;
                    tk_t <= tail[1:0];
                    ch <= 1'b0;
                    pass <= 1'b0;
                    tap <= '0;
                    acc <= '0;
                    state <= ph[32] ? S_PUSH : S_MAC;
                end

            S_PUSH: begin
                ch <= !ch;
                if (ch) begin
                    at <= at + 5'd1;
                    if (cold != '0)
                        cold <= cold - 5'd1;
                    state <= S_MAC;
                end
            end

            S_MAC: begin
                mac_en <= 1'b1;
                mac_add <= pass;
                mac_cap <= pass && tap == '0;
                if (tap == 5'(RSMP_TAPS - 1)) begin
                    tap <= '0;
                    pass <= !pass;
                    if (pass)
                        state <= S_DRAIN;
                end else begin
                    tap <= tap + 5'd1;
                end
            end

            S_DRAIN: state <= S_SCALE;

            S_SCALE: begin
                s_q <= s_hi + RSMP_DIFF_W'({1'b1, s_lo});
                state <= S_SUM;
            end

            S_SUM: begin
                if (!ch) begin
                    pcm_l <= RSMP_OUT_W'(v >>> RSMP_Q);
                    ch <= 1'b1;
                    acc <= '0;
                    state <= S_MAC;
                end else begin
                    pcm_r <= RSMP_OUT_W'(v >>> RSMP_Q);
                    phase <= phase + {1'b0, step_m1} + 33'd1;
                    state <= S_IDLE;
                end
            end

            default: state <= S_IDLE;
        endcase

        if (sst_st_we && sel_st)
            case (sst_addr[2:0])
                3'd1: phase[31:0] <= wdata;
                3'd2: phase[32] <= wdata[30];
                3'd3: {at, cold, pcm_l} <= wdata[27:0];
                3'd4: pcm_r <= wdata[RSMP_OUT_W-1:0];
                default: ;
            endcase
    end

    logic [31:0] st_rd;
    always_comb begin
        case (sst_addr[2:0])
            3'd0: st_rd = step_m1;
            3'd1: st_rd = phase[31:0];
            3'd2: st_rd = {1'b0, phase[32], rst_pend, head, tail, st, b16,
                           open};
            3'd3: st_rd = {4'd0, at, cold, pcm_l};
            3'd4: st_rd = {14'd0, pcm_r};
            default: st_rd = '0;
        endcase
        pcm_quiet = state == S_IDLE;
        pcm_sst_rdata = sel_f ? {fa_q[3], fa_q[2], fa_q[1], fa_q[0]}
            : sel_h ? {16'd0, h_rd}
            : sel_st ? st_rd : '0;
    end

endmodule
