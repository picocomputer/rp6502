/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Nine voices, bit-exact with ria/aud/psg.c: the same walk order, the
 * same accumulator widths, the same wrapping. The two must agree sample
 * for sample, which is what fixes every width below.
 *
 * Eight voices are the program's, out of its XRAM block. The ninth is
 * the bell, six bytes the soft CPU writes, because a bell has to ring
 * whether or not a program installed a pointer.
 *
 * The walk always runs. A parked pointer skips the fetch and nothing
 * else — parking cleared the volumes, and a silent voice makes no sound.
 *
 * One multiplier, one divider, one subtractor, and the voice state in
 * memories the cursor addresses. Under four hundred clocks of the
 * thousand are used, and the spare six hundred are what pays for it.
 *
 * aud_psg_tick is the divider itself, not the end of a walk; everything
 * downstream runs off that pulse.
 */

module aud_psg
    import aud_sine_pkg::*;
#(
    /* The arithmetic's rate, which the envelope tables and phase divisor
     * come out of. Separate from the tick, which only says how often a
     * sample happens: the lockstep shortens that and must not touch this. */
    parameter int RATE = 48000,
    /* 50.4 MHz / 48,000 exactly. */
    parameter int TICKS_PER_SAMPLE = 1050
) (
    input logic clk,

    /* The device register: the sw's validated pointer, 0xFFFF off. */
    input logic xaddr_we,
    input logic [15:0] xaddr_wdata,

    output logic aud_psg_a_req,
    output logic [13:0] aud_psg_a_addr,
    input logic a_gnt,
    input logic [31:0] a_rdata,

    input logic q_we,
    input logic [15:0] q_addr,
    input logic [7:0] q_val,

    /* The bell: a channel block's six bytes in a channel block's order,
     * so the driver writes a descriptor rather than a set of fields. */
    input logic bel_lo_we,
    input logic bel_hi_we,
    input logic [31:0] bel_wdata,

    /* Signed at full scale; the platform narrows. */
    output logic signed [15:0] aud_psg_l,
    output logic signed [15:0] aud_psg_r,
    output logic aud_psg_valid,

    output logic aud_psg_tick
);

    /* The C's entries are all N << 16 with N no wider than nine bits, so
     * nine bits is what is stored and the zeros go back on at the read. A
     * whole word each was sixteen entries of mux to carry seven bits of
     * nothing, twice over — the peak and the sustain are separate reads. */
    localparam logic [8:0] VOL_TABLE[16] = '{
        9'd256, 9'd204, 9'd168, 9'd142,
        9'd120, 9'd102, 9'd86, 9'd73,
        9'd61, 9'd50, 9'd40, 9'd31,
        9'd22, 9'd14, 9'd7, 9'd0
    };
    /* RATE * ms / 1000 as the C does it, not RATE / 1000 * ms, which
     * drops the part of the rate below a kilohertz. 64-bit intermediates:
     * RATE times the longest release passes 2^31 above 89 kHz. Spelled
     * out because Quartus will not take a function call in a localparam
     * array initializer. */
    `define PSG_ENV_RAW(ms) \
        32'd16777216 / 32'((64'(RATE) * 64'd``ms) / 64'd1000)
    /* The shortest attack is the largest step either table holds, so both
     * are sized from it rather than left at the C's int width. */
    localparam logic [31:0] ENV_MAX = `PSG_ENV_RAW(2);
    localparam int ENV_W = $clog2(ENV_MAX + 32'd1);
    `define PSG_ENV(ms) ENV_W'(`PSG_ENV_RAW(ms))
    localparam logic [ENV_W-1:0] ATTACK_TABLE[16] = '{
        `PSG_ENV(2), `PSG_ENV(8), `PSG_ENV(16), `PSG_ENV(24),
        `PSG_ENV(38), `PSG_ENV(56), `PSG_ENV(68), `PSG_ENV(80),
        `PSG_ENV(100), `PSG_ENV(250), `PSG_ENV(500), `PSG_ENV(800),
        `PSG_ENV(1000), `PSG_ENV(3000), `PSG_ENV(5000), `PSG_ENV(8000)
    };
    localparam logic [ENV_W-1:0] DR_TABLE[16] = '{
        `PSG_ENV(6), `PSG_ENV(24), `PSG_ENV(48), `PSG_ENV(72),
        `PSG_ENV(114), `PSG_ENV(168), `PSG_ENV(204), `PSG_ENV(240),
        `PSG_ENV(300), `PSG_ENV(750), `PSG_ENV(1500), `PSG_ENV(2400),
        `PSG_ENV(3000), `PSG_ENV(9000), `PSG_ENV(15000), `PSG_ENV(24000)
    };
    `undef PSG_ENV
    `undef PSG_ENV_RAW

    /* A uint32 in the C, but it never leaves the volume table's range —
     * only the attack's pre-clamp sum passes the largest entry. */
    localparam int VOL_W = $clog2((32'd256 << 16) + ENV_MAX + 32'd1);

    localparam logic [1:0] ADSR_RELEASE = 2'd0;
    localparam logic [1:0] ADSR_ATTACK = 2'd1;
    localparam logic [1:0] ADSR_DECAY = 2'd2;
    localparam logic [1:0] ADSR_SUSTAIN = 2'd3;

    logic [15:0] xaddr;
    logic enabled;
    always_comb enabled = xaddr != 16'hFFFF;

    /* Held to the next walk boundary: a write landing mid-walk would
     * tear the in-flight sample's state. */
    logic xreg_pend;
    logic [15:0] xreg_word;

    /* One array a field, never one wide array: past 136 bits Quartus
     * refuses MLAB, falls back to a block and adds pass-through logic for
     * a read-during-write nobody asked for. Two addresses (w_ch and ch)
     * for the same reason — one address is a single port, which has no
     * asynchronous read to offer.
     *
     * Initialized rather than reset, which is what lets them be memory at
     * all; the zeros ride in the bitstream as a .mif.
     *
     * The gate stays flops: the snoop writes it at whatever index and on
     * whatever clock the 6502 chose, which is a second write port. */
    (* ramstyle = "MLAB, no_rw_check" *)
    logic signed [15:0] ch_sample[9];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [VOL_W-1:0] ch_vol[9];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [31:0] ch_phase[9];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [31:0] ch_noise1[9];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [31:0] ch_noise2[9];
    initial begin
        for (int i = 0; i < 9; i++) begin
            ch_sample[i] = '0;
            ch_vol[i] = '0;
            ch_phase[i] = '0;
            ch_noise1[i] = '0;
            ch_noise2[i] = '0;
        end
    end
    logic [1:0] ch_adsr[9];

    /* Eight entries the walk indexes; MLAB reads asynchronously, so cf
     * stays combinational and no state moves by a cycle. Two 32-bit
     * halves rather than one 64: the fetch arrives a word at a time, and
     * one wide array would need a half-width write enable.
     *
     * Not reset, which is what lets them be memory at all. Nothing reads
     * them before P_FETCH has written all sixteen words. */
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [31:0] cfg_lo[8];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [31:0] cfg_hi[8];
    logic [63:0] cf;
    logic [3:0] ch;

    logic [31:0] bel_lo;
    logic [23:0] bel_hi;
    logic bel_gate_q;
    always_comb cf = ch[3] ? {8'd0, bel_hi, bel_lo}
                           : {cfg_hi[ch[2:0]], cfg_lo[ch[2:0]]};

    /* The address is registered rather than retaken from the cursor: two
     * addresses is a dual port and a dual port is an MLAB. */
    logic [31:0] w_phase, w_noise1, w_noise2;
    logic [VOL_W-1:0] w_vol;
    logic [3:0] w_ch;

    /* The envelope and noise clear, deferred to the load that would have
     * read them: eight entries cannot be cleared on one clock and there
     * is no clock where they must be.
     *
     * The gate is not deferred — a snoop landing between the xreg and the
     * walk has to survive it, which is the C's order. */
    logic clr;
    logic [VOL_W-1:0] ld_vol;
    always_comb ld_vol = clr && !ch[3] ? '0 : ch_vol[ch];

    /* xaddr is 32-bit aligned, so the block is sixteen whole words and no
     * half of one is carried across a fetch. */
    logic cw_en;
    logic [3:0] cw_word;
    logic [31:0] cw_data;
    always_comb begin
        cw_en = gnt_d && fw_c < 5'd16;
        cw_word = 4'(fw_c);
        cw_data = a_rdata;
    end
    logic [15:0] cf_freq;
    logic [7:0] cf_duty, cf_va, cf_vd, cf_wr, cf_pan;
    always_comb begin
        cf_freq = cf[15:0];
        cf_duty = cf[23:16];
        cf_va = cf[31:24];
        cf_vd = cf[39:32];
        cf_wr = cf[47:40];
        cf_pan = cf[55:48];
    end

    /* Decoded where it lands, on the clock it lands. The gate is all the
     * page means today. */
    logic snoop;
    always_comb snoop = q_we && enabled && q_addr[15:8] == xaddr[15:8];
    logic [15:0] snoop_off;
    always_comb snoop_off = q_addr - xaddr;
    logic [2:0] snoop_ch;
    always_comb snoop_ch = snoop_off[5:3];
    logic snoop_gate;
    always_comb snoop_gate = snoop && snoop_off[2:0] == 3'd6
        && snoop_off[15:3] < 13'd8;

    typedef enum logic [2:0] {
        P_IDLE, P_FETCH, P_MIX, P_OUT, P_LOAD, P_DIV, P_PH, P_STEP
    } state_t;
    state_t state;

    logic [12:0] tickctr;
    logic [4:0] fw_i, fw_c;
    logic gnt_d;

    /* Unshifted, rounding once at P_OUT: two truncations in series bias
     * every sounding channel downward, which is DC, not noise. */
    logic signed [26:0] mix_l, mix_r;
    /* The oracle's int8 division truncates toward zero. */
    logic signed [7:0] pan;
    always_comb pan = cf_pan[7]
        ? 8'(($signed(cf_pan) + 8'sd1) >>> 1)
        : 8'($signed(cf_pan) >>> 1);
    /* One multiplier walked three times a channel — the envelope's
     * product, then that against each side's pan. In series on one clock
     * it was the module's longest path, and the walk has seven hundred
     * spare clocks to spend instead.
     *
     * 17x14: sample and mix are both 17 signed, and the widest second
     * operand is the envelope's 13-bit slice. */
    logic [1:0] mix_i;
    logic signed [16:0] mix_s;
    logic signed [16:0] mul_a;
    logic signed [13:0] mul_b;
    logic signed [30:0] mul_p;
    always_comb begin
        mul_a = mix_i == 2'd0 ? 17'(ch_sample[ch]) : mix_s;
        case (mix_i)
            /* Thirteen bits of the Q24 envelope, not nine: at nine a long
             * release moved the gain in 256 visible steps. */
            2'd0: mul_b = $signed({1'b0, ld_vol[VOL_W-1:12]});
            2'd1: mul_b = 14'sd63 - 14'(pan);
            default: mul_b = 14'sd63 + 14'(pan);
        endcase
        mul_p = 31'(mul_a) * 31'(mul_b);
    end

    /* The phase increment: freq * 2^32 over three times the rate.
     *
     * Sized from the divisor rather than assumed. A restoring divider's
     * remainder runs below its divisor and the compare sees it shifted up
     * one bit, so 72000 fitted seventeen and eighteen and 144000 fits
     * neither: at 48 kHz the old widths would have dropped the top bit of
     * every remainder and detuned the whole engine, silently. */
    localparam logic [31:0] PHASE_DIV = 32'(3 * RATE);
    localparam int REM_W = $clog2(PHASE_DIV);
    /* What the shortened division below rests on, said where it can fail
     * loudly. A rate under 21,846 puts the divisor inside sixteen bits and
     * a frequency could then reach it, which would detune the engine
     * quietly rather than not build. */
    initial if (PHASE_DIV <= 32'd65535)
        $fatal(1, "aud_psg: RATE too low for the divider preload");

    /* Thirty-two iterations, not forty-eight: the dividend's top sixteen
     * bits are the frequency, which is below the divisor, so those steps
     * only shift it into the remainder and put zeros in the quotient. */
    logic [31:0] div_q;
    logic [REM_W-1:0] div_rem;
    logic [4:0] div_i;
    logic [REM_W:0] div_t;
    logic [REM_W-1:0] div_next;
    logic div_ge;
    always_comb begin
        div_t = {div_rem, div_q[31]};
        div_ge = div_t >= PHASE_DIV[REM_W:0];
        /* Narrower than div_t on purpose: both arms land below the divisor,
         * so the extra bit is provably zero. */
        div_next = REM_W'(div_ge ? div_t - PHASE_DIV[REM_W:0] : div_t);
    end

    logic [7:0] ph;
    always_comb ph = w_phase[31:24];
    logic [7:0] duty_half;
    always_comb duty_half = cf_duty >> 1;

    /* One comparator for the three waves that rail past the duty, one
     * window for the two that rail either side of the peak. */
    logic past_duty, off_window;
    always_comb begin
        past_duty = ph > cf_duty;
        off_window = ph < 8'd128 - duty_half || ph >= 8'd128 + duty_half;
    end

    /* Both wave constants sit at the top of the word, where the subtraction
     * is wiring and not a carry: 32767 - x is x's low fifteen bits inverted
     * under its own top bit, and x - 32768 is x with its top bit inverted.
     * That is three sixteen-bit subtractors the wave stops asking for. */
    logic signed [15:0] saw, tri_down, tri_up;
    always_comb begin
        saw = {w_phase[31], ~w_phase[30:16]};
        tri_down = {w_phase[30], ~w_phase[29:15]};
        tri_up = {~w_phase[30], w_phase[29:15]};
    end

    /* A quarter table folded back out: a localparam array indexed by a
     * register is a mux of constants, so a quarter of the entries is a
     * quarter of the mux. The fold is exact — the same 256 words the C
     * reads. */
    logic [6:0] sine_a, sine_b;
    logic sine_neg;
    always_comb begin
        sine_a = ph[6:0];
        sine_neg = ph[7] ^ (sine_a > 7'd64);
        sine_b = sine_a > 7'd64 ? 7'(8'd128 - 8'(sine_a)) : sine_a;
    end
    logic signed [15:0] sine;
    always_comb sine = sine_neg ? -$signed(AUD_SINE_Q[sine_b])
                                : $signed(AUD_SINE_Q[sine_b]);

    logic signed [15:0] wave;
    always_comb begin
        case (cf_wr[7:4])
            4'd0: wave = off_window ? -16'sd32767 : sine;
            4'd1: wave = past_duty ? -16'sd32767 : 16'sd32767;
            4'd2: wave = past_duty ? -16'sd32767 : saw;
            4'd3: wave = off_window ? -16'sd32767
                : ph[7] ? tri_down : tri_up;
            4'd4: wave = past_duty ? -16'sd32767
                : $signed(w_noise2[15:0]);
            default: wave = 16'sd0;
        endcase
    end

    /* Noise steps only when its window emits. */
    logic noise_step;
    always_comb noise_step = cf_wr[7:4] == 4'd4 && !past_duty;
    logic [31:0] noise1_x;
    always_comb noise1_x = w_noise1 ^ w_noise2;

    logic [VOL_W-1:0] vol_peak, vol_sus;
    always_comb begin
        vol_peak = VOL_W'({VOL_TABLE[cf_va[7:4]], 16'd0});
        vol_sus = VOL_W'({VOL_TABLE[cf_vd[7:4]], 16'd0});
    end

    /* Decay and release are the same subtraction against different
     * nibbles, so the nibble is what gets muxed. */
    logic [3:0] dr_idx;
    always_comb dr_idx = ch_adsr[ch] == ADSR_DECAY ? cf_vd[3:0] : cf_wr[3:0];
    logic [VOL_W-1:0] dr_val, dr_next;
    always_comb begin
        dr_val = VOL_W'(DR_TABLE[dr_idx]);
        dr_next = w_vol <= dr_val ? '0 : w_vol - dr_val;
    end

    logic [VOL_W-1:0] vol_next;
    logic [1:0] adsr_next;
    always_comb begin
        vol_next = w_vol;
        adsr_next = ch_adsr[ch];
        case (ch_adsr[ch])
            ADSR_ATTACK: begin
                vol_next = w_vol + VOL_W'(ATTACK_TABLE[cf_va[3:0]]);
                if (vol_next >= vol_peak) begin
                    vol_next = vol_peak;
                    adsr_next = ADSR_DECAY;
                end
            end
            ADSR_DECAY: begin
                vol_next = dr_next;
                if (dr_next <= vol_sus) begin
                    adsr_next = ADSR_SUSTAIN;
                    if (vol_sus <= vol_peak)
                        vol_next = vol_sus;
                end
            end
            ADSR_SUSTAIN: begin
                if (vol_sus <= vol_peak)
                    vol_next = vol_sus;
            end
            default: vol_next = dr_next;  /* release */
        endcase
    end

    always_comb begin
        aud_psg_a_req = state == P_FETCH && fw_i < 5'd16;
        aud_psg_a_addr = xaddr[15:2] + {9'd0, fw_i};
    end

    /* Unreset: a reset here would make these flops again. */
    always_ff @(posedge clk) begin
        if (state == P_STEP) begin
            ch_sample[w_ch] <= wave;
            ch_vol[w_ch] <= vol_next;
            ch_phase[w_ch] <= w_phase;
            ch_noise1[w_ch] <= noise_step ? noise1_x : w_noise1;
            ch_noise2[w_ch] <= noise_step ? w_noise2 + noise1_x : w_noise2;
        end
        if (cw_en) begin
            if (cw_word[0]) begin
                cfg_hi[cw_word[3:1]] <= cw_data;
            end else begin
                cfg_lo[cw_word[3:1]] <= cw_data;
            end
        end
    end

    initial begin
        xaddr = 16'hFFFF;
        xreg_pend = 1'b0;
        xreg_word = 16'hFFFF;
        for (int i = 0; i < 9; i++)
            ch_adsr[i] = ADSR_RELEASE;
        clr = 1'b0;
        bel_lo = '0;
        bel_hi = '0;
        bel_gate_q = 1'b0;
        w_phase = '0;
        w_noise1 = '0;
        w_noise2 = '0;
        w_vol = '0;
        w_ch = '0;
        ch = '0;
        state = P_IDLE;
        tickctr = '0;
        fw_i = '0;
        fw_c = '0;
        gnt_d = 1'b0;
        mix_l = '0;
        mix_r = '0;
        mix_i = '0;
        mix_s = '0;
        div_q = '0;
        div_rem = '0;
        div_i = '0;
        aud_psg_l = '0;
        aud_psg_r = '0;
        aud_psg_valid = 1'b0;
        aud_psg_tick = 1'b0;
    end
    always_ff @(posedge clk) begin
        gnt_d <= a_gnt;
        aud_psg_valid <= 1'b0;
        aud_psg_tick <= tickctr == 13'(TICKS_PER_SAMPLE - 1);

        if (tickctr == 13'(TICKS_PER_SAMPLE - 1))
            tickctr <= '0;
        else
            tickctr <= tickctr + 13'd1;

        /* A walk that outlasts its tick drops the sample silently, and
         * the heartbeat the machine rides gets a gap in it. */
        if (tickctr == 13'd0 && state != P_IDLE)
            $fatal(1, "aud_psg walk overrun");

        case (state)
            P_IDLE: begin
                if (xreg_pend) begin
                    /* The phase persists across an xreg; the envelopes
                     * and noise go with clr. */
                    xreg_pend <= 1'b0;
                    xaddr <= xreg_word;
                    clr <= 1'b1;
                    for (int i = 0; i < 8; i++)
                        ch_adsr[i] <= ADSR_RELEASE;
                end
                if (tickctr == 13'd0) begin
                    /* The walk always runs: the bell does not wait on a
                     * program. */
                    ch <= '0;
                    mix_l <= '0;
                    mix_r <= '0;
                    mix_i <= '0;
                    if (xreg_pend ? xreg_word != 16'hFFFF : enabled)
                    begin
                        fw_i <= '0;
                        fw_c <= '0;
                        state <= P_FETCH;
                    end else
                        state <= P_MIX;
                end
            end
            P_FETCH: begin
                if (a_gnt)
                    fw_i <= fw_i + 5'd1;
                if (gnt_d) begin
                    fw_c <= fw_c + 5'd1;
                    if (fw_c == 5'd15) begin
                        ch <= '0;
                        mix_l <= '0;
                        mix_r <= '0;
                        mix_i <= '0;
                        state <= P_MIX;
                    end
                end
            end
            P_MIX: begin
                case (mix_i)
                    2'd0: mix_s <= 17'((mul_p + 31'sd2048) >>> 12);
                    2'd1: if (pan != -8'sd64)
                        mix_l <= mix_l + 27'(mul_p);
                    default: begin
                        if (pan != -8'sd64)
                            mix_r <= mix_r + 27'(mul_p);
                        ch <= ch + 4'd1;
                        if (ch == 4'd8)
                            state <= P_OUT;
                    end
                endcase
                mix_i <= mix_i == 2'd2 ? 2'd0 : mix_i + 2'd1;
            end
            P_OUT: begin
                aud_psg_l <= clamped(21'((mix_l + 27'sd64) >>> 7));
                aud_psg_r <= clamped(21'((mix_r + 27'sd64) >>> 7));
                ch <= '0;
                state <= P_LOAD;
            end
            P_LOAD: begin
                /* cf is already reading the frequency at this address. */
                w_ch <= ch;
                w_phase <= ch_phase[ch];
                w_vol <= ld_vol;
                w_noise1 <= clr && !ch[3] ? 32'h67452301 : ch_noise1[ch];
                w_noise2 <= clr && !ch[3] ? 32'hEFCDAB89 : ch_noise2[ch];
                div_q <= '0;
                div_rem <= REM_W'(cf_freq);
                div_i <= '0;
                state <= P_DIV;
            end
            P_DIV: begin
                div_q <= {div_q[30:0], div_ge};
                div_rem <= div_next;
                div_i <= div_i + 5'd1;
                if (div_i == 5'd31)
                    state <= P_PH;
            end
            P_PH: begin
                /* A clock of its own, so the wave reads a standing
                 * phase rather than a 32-bit add. The machine has under
                 * two nanoseconds spare; this costs eight clocks. */
                w_phase <= w_phase + div_q;
                state <= P_STEP;
            end
            P_STEP: begin
                ch_adsr[ch] <= adsr_next;
                ch <= ch + 4'd1;
                if (ch == 4'd8) begin
                    clr <= 1'b0;
                    aud_psg_valid <= 1'b1;
                    state <= P_IDLE;
                end else
                    state <= P_LOAD;
            end
            default: state <= P_IDLE;
        endcase

        /* After the case, so a write landing on the walk's own step
         * wins it. */
        if (snoop_gate) begin
            if (!q_val[0] && ch_adsr[{1'b0, snoop_ch}] != ADSR_RELEASE)
                ch_adsr[{1'b0, snoop_ch}] <= ADSR_RELEASE;
            if (q_val[0] && ch_adsr[{1'b0, snoop_ch}] == ADSR_RELEASE)
                ch_adsr[{1'b0, snoop_ch}] <= ADSR_ATTACK;
        end

        /* The ninth voice's gate is a level in a register, so its edge
         * is taken here; the other eight get theirs from the snoop.
         * Holding it high must not restrike a note that already died. */
        if (bel_hi[16] && !bel_gate_q && ch_adsr[8] == ADSR_RELEASE)
            ch_adsr[8] <= ADSR_ATTACK;
        if (!bel_hi[16] && bel_gate_q && ch_adsr[8] != ADSR_RELEASE)
            ch_adsr[8] <= ADSR_RELEASE;
        bel_gate_q <= bel_hi[16];

        if (bel_lo_we)
            bel_lo <= bel_wdata;
        if (bel_hi_we)
            bel_hi <= bel_wdata[23:0];

        /* After the case, so a write on an apply clock is kept for
         * the next boundary instead of vanishing under the clear. */
        if (xaddr_we) begin
            xreg_pend <= 1'b1;
            xreg_word <= xaddr_wdata;
        end
    end

    function automatic logic signed [15:0] clamped(logic signed [20:0] s);
        if (s < -21'sd32768)
            s = -21'sd32768;
        if (s > 21'sd32767)
            s = 21'sd32767;
        return 16'(s);
    endfunction

    /* The rest of pan_gate is the pan, and the pan is not edge
     * triggered — it arrives with the rest of the config on the fetch,
     * every sample, and is read out of cf. Only the gate has to be
     * caught at the moment it moves. */
    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_aud_psg;
    always_comb unused_aud_psg = ^{cf[63:56], q_val[7:1], bel_wdata[31:24]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
