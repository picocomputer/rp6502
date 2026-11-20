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
 * one filter after each tick, the OPL2's first. Each is two rows of 48
 * taps, the rows either side of its phase, one multiply-accumulate walked
 * twice and blended as pcm_blend does. That is about 300 of the tick's
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
 * staging pointer is on a word. A commit after the write's last store moves
 * wr to the staging pointer, stage, so the player sees a write whole, as
 * pcm.c's pcm_put publishes wr once. A tick whose phase reaches one takes
 * a frame, or when none is whole each channel's newest sample faded as
 * pcm_fade does. Everything that reads the FIFO and the phase happens on
 * the tick's edge, so a write, clear, open or close lands before a sample
 * or after it. One on the tick's own clock is after it.
 *
 * The histories share one M10K of 128 words: PCM's at 0-63, a frame's two
 * channels in a word, left in the low half, and the OPL2's at 64-127 in
 * the low halves. The player runs on every tick, open or closed, and its
 * history carries on across both, so a filter's tail plays out behind the
 * frames after it, or, closed, fades to silence and stays there.
 *
 * A savestate reads and writes the FIFO through the sst port at savestate
 * offset 0, a row of four bytes a word, PCM's history at 0x600, the
 * OPL2's at 0x7C0, PCM's flops at 0x790 and 0x79A, and the OPL2's at
 * 0x798. Its word
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
    input logic tick,

    /* Bit 0 open, bit 1 16-bit, bit 2 stereo. Opening takes the step
     * written and starts the player's phase over at the next tick. A
     * close empties the FIFO, which by then holds less than a frame. */
    input logic ctrl_we,
    /* Frames per sample, Q32, less one, which the next open takes. */
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
    output logic [31:0] pcm_sst_rdata,
    output logic pcm_quiet
);

    logic sel_f, sel_hp, sel_ho, sel_h, sel_st, sel_so;
    always_comb begin
        sel_f = !sst_addr[10];
        sel_hp = sst_addr[10:6] == 5'h18;
        sel_ho = sst_addr[10:6] == 5'h1F;
        sel_h = sel_hp || sel_ho;
        sel_st = sst_addr[10:3] == 8'hF2;
        sel_so = sst_addr[10:3] == 8'hF3;
    end

    localparam logic [32:0] ONE = 33'h1_0000_0000;
    localparam int DH_W = RSMP_DIFF_W - 16;
    /* A row is the phase's top PB bits, and the blend's fraction the 16
     * after them. */
    localparam int PB = $clog2(RSMP_PHASES);
    /* Each history is a ring of 64. The taps are its newest 48, tap 0 the
     * oldest of them and the last tap the newest. */
    localparam int TB = 6;
    localparam logic [TB-1:0] T_SKIP = TB'(64 - RSMP_TAPS);
    localparam logic [TB-1:0] T_LAST = TB'(RSMP_TAPS - 1);

    localparam longint unsigned O_STEP =
        ((64'(STEP_NUM) << 32) / 64'(STEP_DEN));
    localparam int OPH_W = 34;
    localparam logic [OPH_W-1:0] O_ONE = OPH_W'(64'd1 << 32);
    localparam logic [OPH_W-1:0] O_TWO = OPH_W'(64'd2 << 32);

    logic open, b16, st;
    /* The step written, and the step the player runs at, which an open
     * takes, so a tail playing out keeps its own. */
    logic [31:0] step_in, step_m1;
    logic rst_pend;

    typedef enum logic [2:0] {
        S_IDLE, S_PUSH, S_MAC, S_DRAIN, S_SCALE, S_SUM
    } state_t;
    state_t state /*verilator public_flat_rd*/;

    logic [32:0] phase, ph;
    always_comb ph = rst_pend ? ONE : phase;
    logic go;
    always_comb go = tick && state == S_IDLE;

    /* The OPL's stream. */
    logic [OPH_W-1:0] o_phase;
    logic [TB-1:0] wptr, rptr;
    logic primed, full;
    logic o_go;
    always_comb o_go = step && primed && wptr != rptr && state == S_IDLE;

    /* FIFO: four banks, words aligned. */
    logic [12:0] wr, rd, stage;
    always_comb pcm_level = wr - rd;
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
        w_row = sst_own ? sst_addr[9:0] : stage[11:2];
        for (int b = 0; b < 4; b++) begin
            w_en[b] = sst_own ? sst_we && sel_f
                : data_we || (byte_we && stage[1:0] == 2'(b));
            r_row[b] = rd[11:2] + 10'(2'(b) < rd[1:0]);
        end
    end

    /* The FIFO's four banks, 1024 bytes each in M10K. Port A writes and
     * reads back at w_row, port B is the player's read. In a generate loop, Quartus builds each bank as two RAMs, one
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
    always_ff @(posedge clk) begin
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
    logic [2:0] tk_k;

    /* so: the stream is the OPL's. p_pend: PCM's follow it, p_push with
     * a push first. */
    logic so, p_pend, p_push;
    logic ch, pass;
    logic [TB-1:0] tap;
    logic [TB-1:0] at;
    /* Each channel's newest sample, which an underrun fades. */
    logic signed [15:0] last_l, last_r;

    /* pcm_fade's k: the place of step - 1's top bit above bit 25. It is
     * the stream's, taken with the format at the open, as pcm.c takes it
     * from the step pcm_reset sets. */
    logic [2:0] fk, k_open;
    always_comb begin
        fk = 3'd0;
        for (int b = 26; b < 32; b++)
            if (step_in[b])
                fk = 3'(b - 25);
    end
    function automatic logic signed [15:0] fade(logic signed [15:0] x,
                                               logic [2:0] k);
        logic signed [16:0] up;
        up = 17'(x) + (x > 0 ? 17'((17'd1 << k) - 17'd1) : 17'd0);
        return 16'(17'(x) - (up >>> k));
    endfunction

    /* A frame's two samples, decoded at once. A mono frame's right is its
     * left. */
    logic signed [15:0] dec[2];
    always_comb
        for (int c = 0; c < 2; c++) begin : dec_ch
            logic [1:0] o;
            logic [7:0] lo, hi;
            o = tk_t + (c == 1 && tk_st ? (tk_16 ? 2'd2 : 2'd1) : 2'd0);
            lo = fq[o];
            hi = fq[o + 2'd1];
            if (!tk_has)
                dec[c] = fade(c == 1 ? last_r : last_l, tk_k);
            else if (tk_16)
                dec[c] = {hi, lo};
            else
                dec[c] = {~lo[7], lo[6:0], 8'd0};
        end

    /* The histories, one M10K. The OPL's write wins a clock;
     * a push waits it out. */
    (* ramstyle = "M10K" *)
    logic [31:0] hist[128];
    initial for (int i = 0; i < 128; i++) hist[i] = '0;
    logic [6:0] s_ha;
    always_comb s_ha = {sel_ho, sst_addr[5:0]};
    logic h_we;
    logic [6:0] h_wa;
    logic [31:0] h_wd;
    always_comb begin
        h_we = sst_own ? sst_we && sel_h : in_valid || state == S_PUSH;
        h_wa = sst_own ? s_ha : in_valid ? {1'b1, wptr} : {1'b0, at};
        h_wd = sst_own ? wdata : in_valid ? {16'd0, in_sample}
            : {dec[1], dec[0]};
    end
    always_ff @(posedge clk)
        if (h_we)
            hist[h_wa] <= h_wd;

    logic [TB-1:0] h_b, h_t;
    logic [6:0] e_ra, h_ra;
    always_comb begin
        h_b = so ? rptr : at;
        h_t = h_b + T_SKIP + tap;
        if (so)
            e_ra = {1'b1, !full && h_t >= wptr ? TB'(0) : h_t};
        else
            e_ra = {1'b0, h_t};
        h_ra = sst_own ? s_ha : e_ra;
    end
    /* The word, which half the tap is, and whether it reads zero. */
    logic [31:0] hist_w;
    logic hq_hi;
    always_ff @(posedge clk) begin
        hist_w <= hist[h_ra];
        hq_hi <= !so && ch;
    end
    logic signed [15:0] hist_q;
    always_comb hist_q = hq_hi ? hist_w[31:16] : hist_w[15:0];

    /* The coefficients, one port. */
    localparam int AW = $clog2(RSMP_HALF_N);
    logic signed [RSMP_COEF_W-1:0] coef[RSMP_HALF_N];
    initial begin
        for (int i = 0; i < RSMP_HALF_N; i++) coef[i] = RSMP_HALF[i];
    end
    logic [7:0] c_row, c_r;
    logic [TB-1:0] c_t;
    logic [AW-1:0] c_a;
    logic [PB-1:0] p_row;
    logic [15:0] f;
    always_comb begin
        p_row = so ? o_phase[31-:PB] : phase[31-:PB];
        f = so ? o_phase[31-PB-:16] : phase[31-PB-:16];
        c_row = 8'(p_row) + 8'(pass);
        if (c_row <= 8'(RSMP_PHASES / 2)) begin
            c_r = c_row;
            c_t = tap;
        end else begin
            c_r = 8'(RSMP_PHASES) - c_row;
            c_t = T_LAST - tap;
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
        step_in = '0;
        step_m1 = '0;
        rst_pend = 1'b0;
        state = S_IDLE;
        phase = '0;
        wr = '0;
        rd = '0;
        stage = '0;
        pcm_under = '0;
        tk_has = 1'b0;
        tk_16 = 1'b0;
        tk_st = 1'b0;
        tk_t = '0;
        tk_k = '0;
        k_open = '0;
        so = 1'b0;
        p_pend = 1'b0;
        p_push = 1'b0;
        ch = 1'b0;
        pass = 1'b0;
        tap = '0;
        at = '0;
        last_l = '0;
        last_r = '0;
        hist_w = '0;
        hq_hi = 1'b0;
        mac_en = 1'b0;
        mac_add = 1'b0;
        mac_cap = 1'b0;
        mac_first = 1'b0;
        acc = '0;
        acc_a = '0;
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
        if (ctrl_we && wdata[0]) begin
            k_open <= fk;
            step_m1 <= step_in;
        end
        if (step_we)
            step_in <= wdata;
        if (ctrl_we && wdata[0])
            rst_pend <= 1'b1;
        else if (go)
            rst_pend <= 1'b0;

        if (data_we || byte_we)
            stage <= stage + {10'd0, data_we, 1'b0, !data_we};
        if (commit_we)
            wr <= stage;
        if (go && ph[32] && has)
            rd <= rd + 13'(frame);
        if (clear_we)
            rd <= wr;
        if (under_take)
            pcm_under <= {30'd0, dry};
        else if (dry && !under_inc[31])
            pcm_under <= under_inc[30:0];
        /* A close comes once the FIFO holds less than a frame, and drops
         * that, with anything stored and not committed, so the FIFO is
         * empty, at its start, until the next open. */
        if (ctrl_we && !wdata[0]) begin
            wr <= '0;
            rd <= '0;
            stage <= '0;
        end
        if (ctrl_we && wdata[0])
            pcm_under <= '0;
        if (sst_we && sel_st)
            case (sst_addr[2:0])
                3'd0: step_m1 <= wdata;
                3'd2: {rst_pend, wr, rd, st, b16, open} <= wdata[29:0];
                3'd4: k_open <= wdata[RSMP_OUT_W+2:RSMP_OUT_W];
                3'd5: pcm_under <= wdata[30:0];
                3'd6: stage <= wdata[12:0];
                default: ;
            endcase
        if (sst_we && sel_so && sst_addr[2:0] == 3'd2)
            step_in <= wdata;
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
                rptr <= TB'(1);
                primed <= 1'b1;
            end
            if (&wptr)
                full <= 1'b1;
            wptr <= wptr + TB'(1);
        end

        case (state)
            S_IDLE: begin
                if (go) begin
                    phase <= {1'b0, ph[31:0]};
                    tk_has <= has;
                    tk_16 <= b16;
                    tk_st <= st;
                    tk_t <= rd[1:0];
                    tk_k <= k_open;
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
                    last_l <= dec[0];
                    last_r <= dec[1];
                    at <= at + TB'(1);
                    state <= S_MAC;
                end

            S_MAC: begin
                mac_en <= 1'b1;
                mac_add <= pass;
                mac_first <= !pass && tap == '0;
                mac_cap <= pass && tap == '0;
                if (tap == T_LAST) begin
                    tap <= '0;
                    pass <= !pass;
                    if (pass)
                        state <= S_DRAIN;
                end else begin
                    tap <= tap + TB'(1);
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
                        rptr <= rptr + TB'(2);
                    end else begin
                        o_phase <= ph_sum - O_ONE;
                        rptr <= rptr + TB'(1);
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

        if (sst_we && sel_st)
            case (sst_addr[2:0])
                3'd1: phase[31:0] <= wdata;
                3'd2: phase[32] <= wdata[30];
                3'd3: {at, pcm_l} <= wdata[TB+RSMP_OUT_W-1:0];
                3'd4: pcm_r <= wdata[RSMP_OUT_W-1:0];
                3'd7: {last_r, last_l} <= wdata;
                default: ;
            endcase
        if (sst_we && sel_so)
            case (sst_addr[2:0])
                3'd0: o_phase[31:0] <= wdata;
                3'd1: {o_phase[33:32], wptr, rptr, primed, full, rsmp_out}
                    <= wdata;
                default: ;
            endcase
    end

    logic [31:0] st_rd;
    always_comb begin
        case ({sel_so, sst_addr[2:0]})
            4'h0: st_rd = step_m1;
            4'h1: st_rd = phase[31:0];
            4'h2: st_rd = {1'b0, phase[32], rst_pend, wr, rd, st, b16,
                           open};
            4'h3: st_rd = 32'({at, pcm_l});
            4'h4: st_rd = 32'({k_open, pcm_r});
            4'h5: st_rd = {1'b0, pcm_under};
            4'h6: st_rd = {19'd0, stage};
            4'h7: st_rd = {last_r, last_l};
            4'h8: st_rd = o_phase[31:0];
            4'h9: st_rd = {o_phase[33:32], wptr, rptr, primed, full,
                           rsmp_out};
            4'hA: st_rd = step_in;
            default: st_rd = '0;
        endcase
        pcm_quiet = state == S_IDLE;
        pcm_sst_rdata = sel_f ? fa_q
            : sel_h ? hist_w
            : sel_st || sel_so ? st_rd : '0;
    end

endmodule
