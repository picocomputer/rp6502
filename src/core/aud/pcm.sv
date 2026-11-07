/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Sound resampled to the codec's 48,000 a second: the OPL2's 49,704, and
 * the PCM: device's frames. The OPL2's stream is bit-exact with
 * core/aud/rsmp.c and PCM's with core/aud/pcm.c built with AUD_NATIVE_RATE
 * 48000, off the same generated coefficients.
 *
 * The three streams, the OPL2's and PCM's left and right, take turns on
 * one filter after each tick, the OPL2's first. Each is two rows of 24
 * taps, the rows either side of its phase, one multiply-accumulate walked
 * twice and blended as pcm_blend does. That is about 155 of the tick's
 * 1050 clocks.
 *
 * A YM3812 runs at its own rate or it is not a YM3812, so its samples
 * arrive on in_valid and are resampled onto the tick. 1014 and 1050 off
 * the same 50.4 MHz is exactly 175/169, a phase sequence that cannot
 * drift.
 *
 * The soft CPU writes PCM frames into a 4096-byte FIFO, reading the level
 * first so that it never overruns it. A word lands on a word of the FIFO,
 * and a byte comes in every lane, so the soft CPU sends bytes until the
 * write pointer is on a word. A commit after the write's last store moves
 * the head to the write pointer, so the player sees a write whole, as
 * pcm.c's pcm_put publishes its head once. A tick whose phase reaches one takes a frame, or
 * silence when none is whole. Everything that reads the FIFO and the phase
 * happens on the tick's edge, so a write, clear, open or close lands
 * before a sample or after it. One on the tick's own clock is after it.
 *
 * The histories share one M10K: the OPL2's at 0-31, PCM's at 64-127, and
 * 32-63 never written, so a tap before the first frame reads 32's zero.
 *
 * A savestate reads and writes the FIFO through the sst port at savestate
 * offset 0, a row of four bytes a word, PCM's history at 0x600, the
 * OPL2's at 0x6A0, PCM's flops at 0x790 and the OPL2's at 0x798. Its word
 * comes in on wdata. Between samples every other flop and every registered
 * read is dead, so only that state is kept.
 */

module pcm
    import rsmp_coef_pkg::*;
#(
    /* The two clock divisors, not two frequencies: rounded Hz slips a
     * sample every seven seconds. */
    parameter int STEP_NUM = 1050,
    parameter int STEP_DEN = 1014
) (
    input logic clk,
    input logic clk_sys,
    input logic tick,

    /* Bit 0 open, bit 1 16-bit, bit 2 stereo. Opening empties the FIFO
     * and resets the player at the next tick. */
    input logic ctrl_we,
    /* Frames per sample, Q32, less one. Written while closed. */
    input logic step_we,
    /* Four bytes, wdata[7:0] first, at a word of the FIFO. */
    input logic data_we,
    /* One byte, in every lane of wdata. */
    input logic byte_we,
    input logic clear_we,
    /* Makes the bytes stored since the last commit playable. */
    input logic commit_we,
    input logic [31:0] wdata,
    /* Clears pcm_under on this clock; a take that runs the FIFO dry on the
     * same clock leaves one. */
    input logic under_take,

    output logic [12:0] pcm_level,
    /* Takes that left less than a frame, saturating: each time the FIFO
     * ran dry. An open clears it too. */
    output logic [30:0] pcm_under,
    output logic signed [RSMP_OUT_W-1:0] pcm_l,
    output logic signed [RSMP_OUT_W-1:0] pcm_r,

    /* The OPL2's samples, and the tick it is resampled onto. */
    input logic signed [15:0] in_sample,
    input logic in_valid,
    input logic step,
    output logic signed [15:0] rsmp_out,
    output logic rsmp_valid,

    input logic sst_own,
    input logic [10:0] sst_addr,
    input logic sst_we,
    input logic sst_st_we,
    output logic [31:0] pcm_sst_rdata,
    output logic pcm_quiet
);

    logic sel_f, sel_hp, sel_ho, sel_h, sel_st, sel_so;
    always_comb begin
        sel_f = !sst_addr[10];
        sel_hp = sst_addr[10:6] == 5'h18;
        sel_ho = sst_addr[10:5] == 6'h35;
        sel_h = sel_hp || sel_ho;
        sel_st = sst_addr[10:3] == 8'hF2;
        sel_so = sst_addr[10:3] == 8'hF3;
    end

    localparam logic [32:0] ONE = 33'h1_0000_0000;
    localparam int DH_W = RSMP_DIFF_W - 16;

    localparam longint unsigned O_STEP =
        ((64'(STEP_NUM) << 32) / 64'(STEP_DEN));
    localparam int OPH_W = 34;
    localparam logic [OPH_W-1:0] O_ONE = OPH_W'(64'd1 << 32);
    localparam logic [OPH_W-1:0] O_TWO = OPH_W'(64'd2 << 32);

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

    /* The OPL's stream. */
    logic [OPH_W-1:0] o_phase;
    logic [4:0] wptr, rptr;
    logic primed, full;
    logic o_go;
    always_comb o_go = step && primed && wptr != rptr && state == S_IDLE;

    /* FIFO: four banks, words aligned. */
    logic [12:0] head, tail, wr;
    always_comb pcm_level = head - tail;
    logic [2:0] frame;
    always_comb frame = 3'd1 << (2'(b16) + 2'(st));
    logic has;
    always_comb has = pcm_level >= 13'(frame);
    logic dry;
    always_comb dry = go && ph[32] && has && pcm_level < 13'({frame, 1'b0});
    logic [31:0] under_inc;
    always_comb under_inc = {1'b0, pcm_under} + 32'd1;

    logic [3:0] w_en;
    logic [9:0] w_row, r_row[4];
    always_comb begin
        w_row = sst_own ? sst_addr[9:0] : wr[11:2];
        for (int b = 0; b < 4; b++) begin
            w_en[b] = sst_own ? sst_we && sel_f
                : data_we || (byte_we && wr[1:0] == 2'(b));
            r_row[b] = tail[11:2] + 10'(2'(b) < tail[1:0]);
        end
    end

    /* The FIFO's four banks, 1024 bytes each in M10K. Port A on clk_sys
     * writes and reads back at w_row, port B on clk is the player's
     * read. In a generate loop, Quartus builds each bank as two RAMs, one
     * for each read port. */
    (* ramstyle = "M10K, no_rw_check" *)
    logic [7:0] bank0[1024];
    (* ramstyle = "M10K, no_rw_check" *)
    logic [7:0] bank1[1024];
    (* ramstyle = "M10K, no_rw_check" *)
    logic [7:0] bank2[1024];
    (* ramstyle = "M10K, no_rw_check" *)
    logic [7:0] bank3[1024];
    logic [31:0] fa_q;
    logic [7:0] fq[4];
    always_ff @(posedge clk_sys) begin
        if (w_en[0]) bank0[w_row] <= wdata[7:0];
        if (w_en[1]) bank1[w_row] <= wdata[15:8];
        if (w_en[2]) bank2[w_row] <= wdata[23:16];
        if (w_en[3]) bank3[w_row] <= wdata[31:24];
        fa_q <= {bank3[w_row], bank2[w_row], bank1[w_row], bank0[w_row]};
    end
    always_ff @(posedge clk)
        if (tick) begin
            fq[0] <= bank0[r_row[0]];
            fq[1] <= bank1[r_row[1]];
            fq[2] <= bank2[r_row[2]];
            fq[3] <= bank3[r_row[3]];
        end

    logic tk_has, tk_16, tk_st;
    logic [1:0] tk_t;

    /* so: the stream is the OPL's. p_pend: PCM's follow it, p_push with
     * a push first. */
    logic so, p_pend, p_push;
    logic ch, pass;
    logic [4:0] tap;
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

    /* The histories, one M10K on clk_sys. The OPL's write wins a clock;
     * a push waits it out. */
    (* ramstyle = "M10K" *)
    logic signed [15:0] hist[128];
    initial for (int i = 0; i < 128; i++) hist[i] = '0;
    logic [6:0] s_ha;
    always_comb s_ha = sel_hp ? {1'b1, sst_addr[5:0]} : {2'b00, sst_addr[4:0]};
    logic h_we;
    logic [6:0] h_wa;
    logic signed [15:0] h_wd;
    always_comb begin
        h_we = sst_own ? sst_we && sel_h : in_valid || state == S_PUSH;
        h_wa = sst_own ? s_ha : in_valid ? {2'b00, wptr} : {1'b1, ch, at};
        h_wd = sst_own ? wdata[15:0] : in_valid ? in_sample : dec;
    end
    always_ff @(posedge clk_sys)
        if (h_we)
            hist[h_wa] <= h_wd;

    logic [4:0] h_b, h_t;
    logic [6:0] e_ra, h_ra;
    always_comb begin
        h_b = so ? rptr : at;
        h_t = h_b + 5'(32 - RSMP_TAPS) + tap;
        if (so)
            e_ra = {2'b00, !full && h_t >= wptr ? 5'd0 : h_t};
        else
            e_ra = tap < cold ? 7'h20 : {1'b1, ch, h_t};
        h_ra = sst_own ? s_ha : e_ra;
    end
    logic signed [15:0] hist_q;
    always_ff @(posedge clk_sys) hist_q <= hist[h_ra];

    /* The coefficients, one port. */
    localparam int AW = $clog2(RSMP_HALF_N);
    logic signed [RSMP_COEF_W-1:0] coef[RSMP_HALF_N];
    initial begin
        for (int i = 0; i < RSMP_HALF_N; i++) coef[i] = RSMP_HALF[i];
    end
    logic [7:0] c_row, c_r;
    logic [4:0] c_t;
    logic [AW-1:0] c_a;
    logic [6:0] p_row;
    logic [15:0] f;
    always_comb begin
        p_row = so ? o_phase[31:25] : phase[31:25];
        f = so ? o_phase[24:9] : phase[24:9];
        c_row = 8'(p_row) + 8'(pass);
        if (c_row <= 8'(RSMP_PHASES / 2)) begin
            c_r = c_row;
            c_t = tap;
        end else begin
            c_r = 8'(RSMP_PHASES) - c_row;
            c_t = 5'(RSMP_TAPS - 1) - tap;
        end
        c_a = AW'(c_r) * AW'(RSMP_TAPS) + AW'(c_t);
    end
    logic signed [RSMP_COEF_W-1:0] coef_q;
    always_ff @(posedge clk) coef_q <= coef[c_a];

    /* The first product of a stream loads the accumulator, which is the
     * DSP's own load control. */
    logic mac_en, mac_add, mac_cap, mac_first;
    logic signed [RSMP_DOT_W-1:0] acc, acc_a, prod;
    always_comb prod = RSMP_DOT_W'(coef_q) * RSMP_DOT_W'(hist_q);

    logic signed [RSMP_DIFF_W-1:0] s_q;
    logic signed [DH_W-1:0] dh;
    logic [15:0] dl;
    logic signed [RSMP_DIFF_W-1:0] s_hi;
    logic [15:0] s_lo;
    always_comb begin
        {dh, dl} = acc[RSMP_DIFF_W-1:0];
        s_hi = RSMP_DIFF_W'(dh) * RSMP_DIFF_W'($signed({1'b0, f}));
        s_lo = 16'((32'(dl) * 32'(f)) >> 16);
    end

    /* One adder steps both phases: the OPL2's in its S_SUM, PCM's in the
     * right channel's. */
    logic [OPH_W-1:0] ph_sum;
    always_comb ph_sum = (so ? o_phase : OPH_W'(phase))
        + (so ? OPH_W'(O_STEP) : OPH_W'(step_m1)) + OPH_W'(!so);

    logic signed [RSMP_DOT_W-1:0] v;
    logic signed [RSMP_OUT_W-1:0] vr;
    always_comb begin
        v = RSMP_DOT_W'(s_q) - acc_a;
        vr = RSMP_OUT_W'(v >>> RSMP_Q);
    end

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
        wr = '0;
        pcm_under = '0;
        tk_has = 1'b0;
        tk_16 = 1'b0;
        tk_st = 1'b0;
        tk_t = '0;
        so = 1'b0;
        p_pend = 1'b0;
        p_push = 1'b0;
        ch = 1'b0;
        pass = 1'b0;
        tap = '0;
        at = '0;
        cold = '0;
        mac_en = 1'b0;
        mac_add = 1'b0;
        mac_cap = 1'b0;
        mac_first = 1'b0;
        acc = '0;
        acc_a = '0;
        hist_q = '0;
        coef_q = '0;
        s_q = '0;
        pcm_l = '0;
        pcm_r = '0;
        o_phase = '0;
        wptr = '0;
        rptr = '0;
        primed = 1'b0;
        full = 1'b0;
        rsmp_out = '0;
        rsmp_valid = 1'b0;
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

        if (data_we || byte_we)
            wr <= wr + {10'd0, data_we, 1'b0, !data_we};
        if (commit_we)
            head <= wr;
        if (go && ph[32] && has)
            tail <= tail + 13'(frame);
        if (clear_we)
            tail <= head;
        if (under_take)
            pcm_under <= {30'd0, dry};
        else if (dry && !under_inc[31])
            pcm_under <= under_inc[30:0];
        if (ctrl_we && wdata[0]) begin
            head <= '0;
            tail <= '0;
            wr <= '0;
            pcm_under <= '0;
        end
        if (sst_st_we && sel_st)
            case (sst_addr[2:0])
                3'd0: step_m1 <= wdata;
                3'd2: {rst_pend, head, tail, st, b16, open} <= wdata[29:0];
                3'd5: pcm_under <= wdata[30:0];
                3'd6: wr <= wdata[12:0];
                default: ;
            endcase
    end

    always_ff @(posedge clk) begin
        rsmp_valid <= 1'b0;
        mac_en <= 1'b0;
        mac_cap <= 1'b0;
        if (mac_en)
            acc <= (mac_first ? '0 : acc) + (mac_add ? prod : -prod);
        if (mac_cap)
            acc_a <= acc;

        if (in_valid) begin
            if (!primed) begin
                rptr <= 5'd1;
                primed <= 1'b1;
            end
            if (&wptr)
                full <= 1'b1;
            wptr <= wptr + 5'd1;
        end

        case (state)
            S_IDLE: begin
                if (tick && !open) begin
                    pcm_l <= '0;
                    pcm_r <= '0;
                end
                if (go) begin
                    phase <= {1'b0, ph[31:0]};
                    if (rst_pend)
                        cold <= 5'(RSMP_TAPS);
                    tk_has <= has;
                    tk_16 <= b16;
                    tk_st <= st;
                    tk_t <= tail[1:0];
                end
                if (o_go || go) begin
                    so <= o_go;
                    p_pend <= go;
                    p_push <= ph[32];
                    ch <= 1'b0;
                    pass <= 1'b0;
                    tap <= '0;
                    state <= !o_go && ph[32] ? S_PUSH : S_MAC;
                end
            end

            S_PUSH:
                if (!in_valid) begin
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
                mac_first <= !pass && tap == '0;
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
                if (so) begin
                    rsmp_out <= vr < -18'sd32768 ? -16'sd32768
                        : vr > 18'sd32767 ? 16'sd32767 : 16'(vr);
                    rsmp_valid <= 1'b1;
                    if (ph_sum >= O_TWO) begin
                        o_phase <= ph_sum - O_TWO;
                        rptr <= rptr + 5'd2;
                    end else begin
                        o_phase <= ph_sum - O_ONE;
                        rptr <= rptr + 5'd1;
                    end
                    so <= 1'b0;
                    state <= !p_pend ? S_IDLE : p_push ? S_PUSH : S_MAC;
                end else if (!ch) begin
                    pcm_l <= vr;
                    ch <= 1'b1;
                    state <= S_MAC;
                end else begin
                    pcm_r <= vr;
                    phase <= ph_sum[32:0];
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
        if (sst_st_we && sel_so)
            case (sst_addr[2:0])
                3'd0: o_phase[31:0] <= wdata;
                3'd1: {o_phase[33:32], wptr, rptr, primed, full, rsmp_out}
                    <= wdata[29:0];
                default: ;
            endcase
    end

    logic [31:0] st_rd;
    always_comb begin
        case ({sel_so, sst_addr[2:0]})
            4'h0: st_rd = step_m1;
            4'h1: st_rd = phase[31:0];
            4'h2: st_rd = {1'b0, phase[32], rst_pend, head, tail, st, b16,
                           open};
            4'h3: st_rd = {4'd0, at, cold, pcm_l};
            4'h4: st_rd = {14'd0, pcm_r};
            4'h5: st_rd = {1'b0, pcm_under};
            4'h6: st_rd = {19'd0, wr};
            4'h8: st_rd = o_phase[31:0];
            4'h9: st_rd = {2'd0, o_phase[33:32], wptr, rptr, primed, full,
                           rsmp_out};
            default: st_rd = '0;
        endcase
        pcm_quiet = state == S_IDLE;
        pcm_sst_rdata = sel_f ? fa_q
            : sel_h ? {16'd0, hist_q}
            : sel_st || sel_so ? st_rd : '0;
    end

endmodule
