/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The PSG of ria/aud/psg.c, one sample every four thousand two hundred
 * clocks: eight channels of sine, square, sawtooth, triangle and noise
 * through the 6581's envelope rates. Each sample walks the handler's
 * exact order — the previous sample mixes out under the fresh pan while
 * the phase, wave, and envelope advance behind it. The phase increment
 * divides 2^32 * freq by three times the rate through a restoring divider
 * because the oracle divides, and every accumulator wraps at the C's own
 * width.
 *
 * Register writes arrive from the RW engine's snoop and take effect on
 * the clock they land, which is what a register does. The firmware
 * queues them in a ring and replays the ring at the sample boundary
 * because its processor is elsewhere when the write happens; that ring
 * is a fact about the emulation and not about the machine, and carrying
 * it here bought a 256-entry memory, a drain state, and a bound of
 * thirty-two writes a sample that hardware has no reason to have.
 *
 * No bell here. There is one for the whole machine, past the engine
 * mux in rp6502.sv, because only one engine is ever audible and two
 * instances meant paying for a second that could not be heard. When the
 * pointer is parked the walk still runs and emits silence, so the
 * sample tick the output stage rides never stops.
 */

module aud_psg
    import aud_sine_pkg::*;
#(
    /* Samples per second the arithmetic is built for: the envelope tables
     * and the phase divisor come out of this. Separate from the tick below,
     * which only decides how often a sample happens — the lockstep shortens
     * that to run faster and must not change this. psg_setup is handed the
     * same number; PSG_SHIM_RATE is where the test states it. */
    parameter int RATE = 48000,
    /* 50.4 MHz / 48,000 exactly. */
    parameter int TICKS_PER_SAMPLE = 1050
) (
    input logic clk,
    input logic rst_n,

    /* The device register: the sw's validated pointer, 0xFFFF off. */
    input logic xaddr_we,
    input logic [15:0] xaddr_wdata,

    /* XRAM port A, one rotor slot. */
    output logic aud_psg_a_req,
    output logic [13:0] aud_psg_a_addr,
    input logic a_gnt,
    input logic [31:0] a_rdata,

    /* The RW engine's write snoop. */
    input logic q_we,
    input logic [15:0] q_addr,
    input logic [7:0] q_val,

    /* One stereo sample per tick, signed at full scale. The platform
     * narrows: the Pocket's I2S takes all sixteen bits. */
    output logic signed [15:0] aud_psg_l,
    output logic signed [15:0] aud_psg_r,
    output logic aud_psg_valid
);

    localparam logic [31:0] VOL_TABLE[16] = '{
        32'd256 << 16, 32'd204 << 16, 32'd168 << 16, 32'd142 << 16,
        32'd120 << 16, 32'd102 << 16, 32'd86 << 16, 32'd73 << 16,
        32'd61 << 16, 32'd50 << 16, 32'd40 << 16, 32'd31 << 16,
        32'd22 << 16, 32'd14 << 16, 32'd7 << 16, 32'd0
    };
    /* psg_setup's tables, elaborated: (1 << 24) over the samples in that
     * many milliseconds, with the milliseconds converted as RATE * ms / 1000
     * the way the C does — not RATE / 1000 * ms, which drops the part of the
     * rate below a kilohertz. Sixty-four bit intermediates because RATE
     * times the longest release passes 2^31 above 89 kHz.
     *
     * Spelled out rather than folded into a constant function: this has to
     * elaborate in Quartus as well as Verilator, and a function call in a
     * localparam array initializer is the sort of thing only one of them
     * accepts. */
    `define PSG_ENV(ms) 32'd16777216 / 32'((64'(RATE) * 64'd``ms) / 64'd1000)
    localparam logic [31:0] ATTACK_TABLE[16] = '{
        `PSG_ENV(2), `PSG_ENV(8), `PSG_ENV(16), `PSG_ENV(24),
        `PSG_ENV(38), `PSG_ENV(56), `PSG_ENV(68), `PSG_ENV(80),
        `PSG_ENV(100), `PSG_ENV(250), `PSG_ENV(500), `PSG_ENV(800),
        `PSG_ENV(1000), `PSG_ENV(3000), `PSG_ENV(5000), `PSG_ENV(8000)
    };
    localparam logic [31:0] DR_TABLE[16] = '{
        `PSG_ENV(6), `PSG_ENV(24), `PSG_ENV(48), `PSG_ENV(72),
        `PSG_ENV(114), `PSG_ENV(168), `PSG_ENV(204), `PSG_ENV(240),
        `PSG_ENV(300), `PSG_ENV(750), `PSG_ENV(1500), `PSG_ENV(2400),
        `PSG_ENV(3000), `PSG_ENV(9000), `PSG_ENV(15000), `PSG_ENV(24000)
    };
    `undef PSG_ENV

    localparam logic [1:0] ADSR_RELEASE = 2'd0;
    localparam logic [1:0] ADSR_ATTACK = 2'd1;
    localparam logic [1:0] ADSR_DECAY = 2'd2;
    localparam logic [1:0] ADSR_SUSTAIN = 2'd3;

    logic [15:0] xaddr;
    logic enabled;
    always_comb enabled = xaddr != 16'hFFFF;

    /* psg_xreg cannot preempt the ISR, so a device-register write holds
     * here and takes effect at the next walk boundary; a write landing
     * mid-walk must not tear the in-flight sample's state. */
    logic xreg_pend;
    logic [15:0] xreg_word;

    /* The handler reads the pointer once at entry — psg_xreg can't
     * interleave with the ISR — so a walk keeps the pointer and flavor
     * it started with. */
    logic walk_psg;
    logic [15:0] walk_xaddr;

    /* Channel state, the C's exactly. */
    logic signed [15:0] ch_sample[8];
    logic [1:0] ch_adsr[8];
    logic [31:0] ch_vol[8];
    logic [31:0] ch_phase[8];
    logic [31:0] ch_noise1[8];
    logic [31:0] ch_noise2[8];

    /* The 64 config bytes, gathered fresh each sample. */
    logic [543:0] gather;
    logic [511:0] cview;
    always_comb cview = 512'(gather >> {walk_xaddr[1], 4'b0000});
    logic [63:0] cf;
    logic [2:0] ch;
    always_comb cf = cview[{3'(ch), 6'd0}+:64];
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

    /* The write snoop, decoded where it lands. The firmware keeps a
     * ring of every write to the device page and replays it at the
     * sample boundary because a processor cannot be in two places at
     * once; this one sees the write on the clock it happens and has
     * nowhere to put it but the state it affects. Anything the page
     * means is decoded here, and today the channel's gate is all it
     * means. */
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
        P_IDLE, P_FETCH, P_MIX, P_OUT, P_DIV, P_STEP
    } state_t;
    state_t state;

    logic [12:0] tickctr;
    logic [4:0] fw_i, fw_c, fw_n;
    always_comb fw_n = walk_xaddr[1] ? 5'd17 : 5'd16;
    logic gnt_d;

    /* The output mix accumulates unshifted in the C's int32 and rounds
     * once at P_OUT. Two truncations in series used to bias every
     * sounding channel downward, which is DC, not noise. */
    logic signed [26:0] mix_l, mix_r;
    /* The oracle's int8 division truncates toward zero. */
    logic signed [7:0] pan;
    always_comb pan = cf_pan[7]
        ? 8'(($signed(cf_pan) + 8'sd1) >>> 1)
        : 8'($signed(cf_pan) >>> 1);
    /* Thirteen bits of the Q24 envelope, not nine: at nine a long release
     * moved the gain in 256 visible steps. Rounded, like the C. */
    logic signed [16:0] mix_s;
    always_comb mix_s =
        17'((30'($signed(ch_sample[ch]) * $signed({1'b0, ch_vol[ch][24:12]}))
             + 30'sd2048) >>> 12);

    /* The pan multiply, widened before it happens rather than after: the
     * product needs every bit of 27 and a 17-bit context would drop it. */
    logic signed [26:0] pan_l, pan_r;
    always_comb begin
        pan_l = 27'(mix_s) * (27'sd63 - 27'(pan));
        pan_r = 27'(mix_s) * (27'sd63 + 27'(pan));
    end

    /* One restoring divider makes the phase increment: the 48-bit
     * dividend freq * 2^32 over three times the rate.
     *
     * Sized from the divisor rather than assumed. A restoring divider's
     * remainder runs below its divisor and the compare sees it shifted up
     * one bit, so 72000 fitted seventeen and eighteen and 144000 fits
     * neither: at 48 kHz the old widths would have dropped the top bit of
     * every remainder and detuned the whole engine, silently. */
    localparam logic [31:0] PHASE_DIV = 32'(3 * RATE);
    localparam int REM_W = $clog2(PHASE_DIV);

    logic [47:0] div_q;
    logic [REM_W-1:0] div_rem;
    logic [5:0] div_i;
    logic [REM_W:0] div_t;
    logic [REM_W-1:0] div_next;
    logic div_ge;
    always_comb begin
        div_t = {div_rem, div_q[47]};
        div_ge = div_t >= PHASE_DIV[REM_W:0];
        /* Narrower than div_t on purpose: both arms land below the divisor,
         * so the extra bit is provably zero. */
        div_next = REM_W'(div_ge ? div_t - PHASE_DIV[REM_W:0] : div_t);
    end

    logic [31:0] phase_next;
    always_comb phase_next = ch_phase[ch] + div_q[31:0];
    logic [7:0] ph;
    always_comb ph = phase_next[31:24];
    logic [7:0] duty_half;
    always_comb duty_half = cf_duty >> 1;

    /* The wave sample from the advanced phase. */
    logic signed [15:0] wave;
    always_comb begin
        case (cf_wr[7:4])
            4'd0: begin
                if (ph < 8'd128 - duty_half || ph >= 8'd128 + duty_half)
                    wave = -16'sd32767;
                else
                    wave = $signed(AUD_SINE[ph]);
            end
            4'd1: wave = ph > cf_duty ? -16'sd32767 : 16'sd32767;
            4'd2: wave = ph > cf_duty ? -16'sd32767
                : 16'(16'sd32767 - $signed({1'b0, phase_next[31:16]}));
            4'd3: begin
                if (ph < 8'd128 - duty_half || ph >= 8'd128 + duty_half)
                    wave = -16'sd32767;
                else if (ph >= 8'd128)
                    wave = 16'(16'sd32767 - $signed(phase_next[30:15]));
                else
                    wave = 16'($signed(phase_next[30:15]) - 16'sd32768);
            end
            4'd4: wave = ph > cf_duty ? -16'sd32767
                : $signed(ch_noise2[ch][15:0]);
            default: wave = 16'sd0;
        endcase
    end

    /* Noise steps only when its window emits: the xor lands first, the
     * sample is noise2's low byte, then noise2 absorbs the sum. */
    logic noise_step;
    always_comb noise_step = cf_wr[7:4] == 4'd4 && !(ph > cf_duty);
    logic [31:0] noise1_x;
    always_comb noise1_x = ch_noise1[ch] ^ ch_noise2[ch];

    /* The envelope's next value. */
    logic [31:0] vol_next;
    logic [1:0] adsr_next;
    always_comb begin
        vol_next = ch_vol[ch];
        adsr_next = ch_adsr[ch];
        case (ch_adsr[ch])
            ADSR_ATTACK: begin
                vol_next = ch_vol[ch] + ATTACK_TABLE[cf_va[3:0]];
                if (vol_next >= VOL_TABLE[cf_va[7:4]]) begin
                    vol_next = VOL_TABLE[cf_va[7:4]];
                    adsr_next = ADSR_DECAY;
                end
            end
            ADSR_DECAY: begin
                if (ch_vol[ch] <= DR_TABLE[cf_vd[3:0]])
                    vol_next = 32'd0;
                else
                    vol_next = ch_vol[ch] - DR_TABLE[cf_vd[3:0]];
                if (vol_next <= VOL_TABLE[cf_vd[7:4]]) begin
                    adsr_next = ADSR_SUSTAIN;
                    if (VOL_TABLE[cf_vd[7:4]] <= VOL_TABLE[cf_va[7:4]])
                        vol_next = VOL_TABLE[cf_vd[7:4]];
                end
            end
            ADSR_SUSTAIN: begin
                if (VOL_TABLE[cf_vd[7:4]] <= VOL_TABLE[cf_va[7:4]])
                    vol_next = VOL_TABLE[cf_vd[7:4]];
            end
            default: begin  /* release */
                if (ch_vol[ch] <= DR_TABLE[cf_wr[3:0]])
                    vol_next = 32'd0;
                else
                    vol_next = ch_vol[ch] - DR_TABLE[cf_wr[3:0]];
            end
        endcase
    end

    always_comb begin
        aud_psg_a_req = state == P_FETCH && {1'b0, fw_i} < {1'b0, fw_n};
        aud_psg_a_addr = walk_xaddr[15:2] + {9'd0, fw_i};
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            xaddr <= 16'hFFFF;
            xreg_pend <= 1'b0;
            xreg_word <= 16'hFFFF;
            for (int i = 0; i < 8; i++) begin
                ch_sample[i] <= '0;
                ch_adsr[i] <= ADSR_RELEASE;
                ch_vol[i] <= '0;
                ch_phase[i] <= '0;
                ch_noise1[i] <= '0;
                ch_noise2[i] <= '0;
            end
            gather <= '0;
            ch <= '0;
            state <= P_IDLE;
            tickctr <= '0;
            fw_i <= '0;
            fw_c <= '0;
            gnt_d <= 1'b0;
            walk_psg <= 1'b0;
            walk_xaddr <= 16'hFFFF;
            mix_l <= '0;
            mix_r <= '0;
            div_q <= '0;
            div_rem <= '0;
            div_i <= '0;
            aud_psg_l <= '0;
            aud_psg_r <= '0;
            aud_psg_valid <= 1'b0;
        end else begin
            gnt_d <= a_gnt;
            aud_psg_valid <= 1'b0;

            if (tickctr == 13'(TICKS_PER_SAMPLE - 1))
                tickctr <= '0;
            else
                tickctr <= tickctr + 13'd1;

            case (state)
                P_IDLE: begin
                    if (xreg_pend) begin
                        /* psg_xreg at the boundary: the pointer lands,
                         * the envelopes and noise reset; the phase
                         * persists. */
                        xreg_pend <= 1'b0;
                        xaddr <= xreg_word;
                        for (int i = 0; i < 8; i++) begin
                            ch_noise1[i] <= 32'h67452301;
                            ch_noise2[i] <= 32'hEFCDAB89;
                            ch_vol[i] <= '0;
                            ch_adsr[i] <= ADSR_RELEASE;
                        end
                    end
                    if (tickctr == 13'd0) begin
                        walk_psg <= xreg_pend ? xreg_word != 16'hFFFF
                                              : enabled;
                        walk_xaddr <= xreg_pend ? xreg_word : xaddr;
                        if (xreg_pend ? xreg_word != 16'hFFFF : enabled)
                        begin
                            fw_i <= '0;
                            fw_c <= '0;
                            gather <= '0;
                            state <= P_FETCH;
                        end else
                            state <= P_OUT;
                    end
                end
                P_FETCH: begin
                    if (a_gnt)
                        fw_i <= fw_i + 5'd1;
                    if (gnt_d) begin
                        case (fw_c)
                            5'd0: gather[31:0] <= a_rdata;
                            5'd1: gather[63:32] <= a_rdata;
                            5'd2: gather[95:64] <= a_rdata;
                            5'd3: gather[127:96] <= a_rdata;
                            5'd4: gather[159:128] <= a_rdata;
                            5'd5: gather[191:160] <= a_rdata;
                            5'd6: gather[223:192] <= a_rdata;
                            5'd7: gather[255:224] <= a_rdata;
                            5'd8: gather[287:256] <= a_rdata;
                            5'd9: gather[319:288] <= a_rdata;
                            5'd10: gather[351:320] <= a_rdata;
                            5'd11: gather[383:352] <= a_rdata;
                            5'd12: gather[415:384] <= a_rdata;
                            5'd13: gather[447:416] <= a_rdata;
                            5'd14: gather[479:448] <= a_rdata;
                            5'd15: gather[511:480] <= a_rdata;
                            5'd16: gather[543:512] <= a_rdata;
                            default: ;
                        endcase
                        fw_c <= fw_c + 5'd1;
                        if (fw_c + 5'd1 == fw_n) begin
                            ch <= '0;
                            mix_l <= '0;
                            mix_r <= '0;
                            state <= P_MIX;
                        end
                    end
                end
                P_MIX: begin
                    if (pan != -8'sd64) begin
                        mix_l <= mix_l + pan_l;
                        mix_r <= mix_r + pan_r;
                    end
                    ch <= ch + 3'd1;
                    if (ch == 3'd7)
                        state <= P_OUT;
                end
                P_OUT: begin
                    if (walk_psg) begin
                        aud_psg_l <= clamped(21'((mix_l + 27'sd64) >>> 7));
                        aud_psg_r <= clamped(21'((mix_r + 27'sd64) >>> 7));
                        ch <= '0;
                        div_q <= {cf_freq, 32'd0};
                        div_rem <= '0;
                        div_i <= '0;
                        state <= P_DIV;
                    end else begin
                        /* No program: silence, but keep the sample tick
                         * so the output stage's bell still reaches the
                         * codec. bel_irq_handler's counterpart. */
                        aud_psg_l <= '0;
                        aud_psg_r <= '0;
                        aud_psg_valid <= 1'b1;
                        state <= P_IDLE;
                    end
                end
                P_DIV: begin
                    div_q <= {div_q[46:0], div_ge};
                    div_rem <= div_next;
                    div_i <= div_i + 6'd1;
                    if (div_i == 6'd47)
                        state <= P_STEP;
                end
                P_STEP: begin
                    ch_phase[ch] <= phase_next;
                    ch_sample[ch] <= wave;
                    if (noise_step) begin
                        ch_noise1[ch] <= noise1_x;
                        ch_noise2[ch] <= ch_noise2[ch] + noise1_x;
                    end
                    ch_vol[ch] <= vol_next;
                    ch_adsr[ch] <= adsr_next;
                    ch <= ch + 3'd1;
                    if (ch == 3'd7) begin
                        aud_psg_valid <= 1'b1;
                        state <= P_IDLE;
                    end else begin
                        div_q <= {cview[{3'(ch + 3'd1), 6'd0}+:16], 32'd0};
                        div_rem <= '0;
                        div_i <= '0;
                        state <= P_DIV;
                    end
                end
                default: state <= P_IDLE;
            endcase

            /* After the case, so a write landing on the same clock as
             * the walk's own step wins it. The program has just said
             * what the channel is doing; the envelope is only saying
             * what it was doing. */
            if (snoop_gate) begin
                if (!q_val[0] && ch_adsr[snoop_ch] != ADSR_RELEASE)
                    ch_adsr[snoop_ch] <= ADSR_RELEASE;
                if (q_val[0] && ch_adsr[snoop_ch] == ADSR_RELEASE)
                    ch_adsr[snoop_ch] <= ADSR_ATTACK;
            end

            /* After the case, so a write on an apply clock is kept for
             * the next boundary instead of vanishing under the clear. */
            if (xaddr_we) begin
                xreg_pend <= 1'b1;
                xreg_word <= xaddr_wdata;
            end
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
    always_comb unused_aud_psg = ^{cf[63:56], gather, cview,
                                   walk_xaddr[0], q_val[7:1]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
