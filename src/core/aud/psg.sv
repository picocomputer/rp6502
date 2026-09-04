/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Nine voices, bit-exact with core/aud/psg.c: the same walk order,
 * accumulator widths and wrapping. That agreement is what fixes every
 * width below.
 *
 * Eight voices are the program's. Their registers arrive as writes and
 * are held here, so the engine never reads XRAM. Only the 6502's writes
 * strike a gate; the soft CPU's import carries the same bytes and must
 * start nothing. The ninth voice is the bell, which has to ring whether
 * or not a program installed a pointer.
 *
 * One multiplier for the mix, one for the phase, a subtractor, and the
 * voice state in memories the cursor addresses. The constants are
 * memories too, read a clock ahead of the step that spends them: a
 * table indexed by a register is a mux, and these were the largest
 * logic in the module before they were a read.
 *
 * psg_tick is the sample divider, not the end of a walk.
 */

module psg
    import aud_sine_pkg::*;
#(
    /* The arithmetic's rate. Separate from the tick, which only says how
     * often a sample happens: the lockstep shortens that, not this. */
    parameter int RATE = 48000,
    /* 50.4 MHz / 48,000 exactly. */
    parameter int TICKS_PER_SAMPLE = 1050
) (
    input logic clk,

    /* The device register: the sw's validated pointer, 0xFFFF off. */
    input logic xaddr_we,
    input logic [15:0] xaddr_wdata,

    /* The restore's key. A gate is an edge and only the 6502 makes one,
     * which is right for a program and wrong for the one thing that is
     * not one: the firmware replaying a channel block after a wake
     * carries the same bytes, gate bit and all, and without this the
     * note in them never strikes. A voice that was sounding when the
     * blob was taken then stays silent for the rest of the program's
     * life -- not for an envelope, for good. Held for the replay and
     * dropped after it. */
    input logic gate_any_we,
    input logic gate_any_wdata,

    /* Every write to XRAM. q_host marks the 6502's own, which are the
     * only ones a gate answers. */
    input logic q_we,
    input logic q_host,
    input logic [15:0] q_addr,
    input logic [7:0] q_val,

    /* A channel block's six bytes in a channel block's order. */
    input logic bel_lo_we,
    input logic bel_hi_we,
    input logic [31:0] bel_wdata,

    /* Signed at full scale; the platform narrows. */
    output logic signed [15:0] psg_l,
    output logic signed [15:0] psg_r,
    output logic psg_valid,

    output logic psg_tick
);

    /* The C's entries are all N << 16 with N no wider than nine bits, so
     * nine bits is stored and the zeros go back on at the read. */
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
    /* The shortest attack is the largest step either table holds. */
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

    /* The pointer moves on the write, because the import that follows has
     * to land inside the new window. What it resets is held to the next
     * walk boundary; a clear landing mid-walk would tear the sample. */
    logic xreg_pend;

    logic [3:0] ch;

    /* Software rejects an odd pointer and a block crossing its page, so a
     * write inside the page and under sixty-four of the base can only be
     * a write to the structure. */
    logic snoop;
    always_comb snoop = q_we && enabled && q_addr[15:8] == xaddr[15:8];
    logic [15:0] snoop_off;
    always_comb snoop_off = q_addr - xaddr;
    logic [2:0] snoop_ch;
    always_comb snoop_ch = snoop_off[5:3];
    logic snoop_cfg;
    always_comb snoop_cfg = snoop && snoop_off[15:6] == 10'd0;
    /* A gate is an edge, and only the 6502 makes one -- or the restore
     * below, which is the firmware standing in for the 6502 that made
     * the last one before the sleep. */
    logic gate_any;
    initial gate_any = 1'b0;
    logic snoop_gate;
    always_comb snoop_gate = snoop_cfg && (q_host || gate_any)
        && snoop_off[2:0] == 3'd6;

    /* One array a field, never one wide array: a wide enough array runs
     * past LUT memory and falls back to a block. Two addresses (w_ch and
     * ch) because one address is a single port, which has no
     * asynchronous read to offer.
     *
     * Initialized rather than reset, which is what lets them be memory at
     * all. The gate stays flops: the snoop writes it at whatever index
     * and clock the 6502 chose, which is a second write port. */
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

    /* A byte of the structure to an array, for the reason above: writes
     * arrive a byte at a time and one wide array would need a byte's
     * worth of write enable. Read asynchronously, so cf stays
     * combinational. The structure's seventh byte is padding.
     *
     * Initialization matters here: the walk reads these from the first
     * sample, before any program has written them. */
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [7:0] cfg_freq_lo[8];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [7:0] cfg_freq_hi[8];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [7:0] cfg_duty[8];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [7:0] cfg_va[8];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [7:0] cfg_vd[8];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [7:0] cfg_wr[8];
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [7:0] cfg_pan[8];
    initial begin
        for (int i = 0; i < 8; i++) begin
            cfg_freq_lo[i] = '0;
            cfg_freq_hi[i] = '0;
            cfg_duty[i] = '0;
            cfg_va[i] = '0;
            cfg_vd[i] = '0;
            cfg_wr[i] = '0;
            cfg_pan[i] = '0;
        end
    end
    logic [63:0] cf;

    logic [31:0] bel_lo;
    logic [23:0] bel_hi;
    logic bel_gate_q;
    /* A write to the voice the cursor stands on is forwarded around the
     * array it lands in: an unforwarded read of an array being written is
     * undefined in the silicon, and one bad byte of wave_release latches
     * an envelope that never releases. */
    logic fwd;
    always_comb fwd = snoop_cfg && !ch[3] && snoop_ch == ch[2:0];
    logic [63:0] cfg_word;
    always_comb begin
        cfg_word = {8'd0, cfg_pan[ch[2:0]], cfg_wr[ch[2:0]], cfg_vd[ch[2:0]],
                    cfg_va[ch[2:0]], cfg_duty[ch[2:0]], cfg_freq_hi[ch[2:0]],
                    cfg_freq_lo[ch[2:0]]};
        if (fwd)
            cfg_word[{snoop_off[2:0], 3'd0}+:8] = q_val;
    end
    always_comb cf = ch[3] ? {8'd0, bel_hi, bel_lo} : cfg_word;

    /* The address is registered rather than retaken from the cursor: two
     * addresses is a dual port and a dual port is an MLAB. */
    logic [31:0] w_phase, w_noise1, w_noise2;
    logic [31:0] w_inc;
    logic [VOL_W-1:0] w_vol;
    logic [3:0] w_ch;

    /* Deferred to the load that would have read them: eight entries
     * cannot be cleared on one clock. The gate is not deferred — a snoop
     * landing between the xreg and the walk has to survive it. */
    logic clr;
    logic [VOL_W-1:0] ld_vol;
    always_comb ld_vol = clr && !ch[3] ? '0 : ch_vol[ch];

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

    typedef enum logic [2:0] {
        P_IDLE, P_MIX, P_OUT, P_LOAD, P_PH, P_STEP
    } state_t;
    state_t state;

    logic [12:0] tickctr;

    /* Rounded once: two truncations in series bias every sounding channel
     * downward, which is DC, not noise. Carried in at the start of the
     * walk, where it is a constant the accumulators load rather than an
     * adder they feed. */
    localparam logic signed [26:0] MIX_ROUND = 27'sd64;
    logic signed [26:0] mix_l, mix_r;
    /* The oracle's int8 division truncates toward zero. Taken once per
     * channel, on the clock its envelope is: a write landing between the
     * two sides would otherwise place the same voice twice. */
    logic signed [7:0] pan_c;
    always_comb pan_c = cf_pan[7]
        ? 8'(($signed(cf_pan) + 8'sd1) >>> 1)
        : 8'($signed(cf_pan) >>> 1);
    logic signed [7:0] pan;
    /* One multiplier walked three times a channel: the envelope's
     * product, then that against each side's pan. In series on one clock
     * it was the module's longest path.
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

    /* The phase increment: freq * 2^32 over three times the rate, which
     * the oracle divides for and this multiplies for.
     *
     * 144000 is 2^7 * 1125 and (2^51 + 127) / 1125 is a whole number, so
     * freq * that, shifted down twenty-six, is exactly floor(freq * 2^32
     * / 144000) for every frequency. The remainder 127 is what makes the
     * shift twenty-six: the accumulated error is freq * 127, and
     * 65535 * 127 must stay under the shift. No narrower constant is
     * exact, and the test walks all 65536 against a real division.
     *
     * Forty-one bits, so this is a DSP and must stay one. */
    localparam logic [31:0] PHASE_DIV = 32'(3 * RATE);
    localparam logic [40:0] PHASE_MAGIC = 41'd2001599834387;
    localparam int PHASE_SHIFT = 26;
    /* The constant above is 48 kHz's; a rate that moves without it
     * detunes the engine silently. */
    initial if (PHASE_DIV != 32'd144000)
        $fatal(1, "psg: RATE moved and the phase magic did not");
    initial if (64'(PHASE_MAGIC) * 64'd1125 != (64'd1 << 51) + 64'd127)
        $fatal(1, "psg: phase magic is not (2^51 + 127) / 1125");

    (* multstyle = "dsp" *)
    logic [56:0] inc_prod;
    always_comb inc_prod = 57'(cf_freq) * 57'(PHASE_MAGIC);

    /* The block above is addressed from these same wires, so the read and
     * the register move together. */
    logic [31:0] ph_next;
    always_comb ph_next = w_phase + w_inc;
    logic [ROM_W-1:0] sine_q, env_q;

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

    /* Wiring, not a carry: 32767 - x is x's low fifteen bits inverted
     * under its own top bit, and x - 32768 is x with its top bit
     * inverted. Three sixteen-bit subtractors saved. */
    logic signed [15:0] saw, tri_down, tri_up;
    always_comb begin
        saw = {w_phase[31], ~w_phase[30:16]};
        tri_down = {w_phase[30], ~w_phase[29:15]};
        tri_up = {~w_phase[30], w_phase[29:15]};
    end

    /* The circle in a block, addressed off the phase adder rather than
     * the register it lands in, so the word is waiting and the walk grows
     * no state. Initialized, not written: it rides in as a .mif.
     *
     * The envelope's two rate tables share the block above the circle,
     * attack at 256 and decay-and-release at 272. A voice reads one or
     * the other, never both, so they share the second port.
     *
     * The rate is fetched a clock ahead of the step, so a gate landing
     * exactly on the fetch turns the arm the step reads but not the
     * address the fetch used: that one step moves at the old arm's rate. */
    localparam int ROM_W = 20;
    localparam int ROM_ATK = 256;
    localparam int ROM_DR = 272;
    (* ramstyle = "M10K, no_rw_check" *)
    logic [ROM_W-1:0] aud_rom[512];
    initial begin
        for (int i = 0; i < 512; i++)
            aud_rom[i] = '0;
        for (int i = 0; i < 256; i++)
            aud_rom[i] = ROM_W'(AUD_SINE[i]);
        for (int i = 0; i < 16; i++) begin
            aud_rom[ROM_ATK+i] = ROM_W'(ATTACK_TABLE[i]);
            aud_rom[ROM_DR+i] = ROM_W'(DR_TABLE[i]);
        end
    end
    logic signed [15:0] sine;
    always_comb sine = $signed(sine_q[15:0]);

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
    logic [8:0] env_addr;
    always_comb env_addr = ch_adsr[ch] == ADSR_ATTACK
        ? 9'(ROM_ATK) + {5'd0, cf_va[3:0]}
        : 9'(ROM_DR) + {5'd0, dr_idx};
    logic [VOL_W-1:0] dr_val, dr_next;
    always_comb begin
        dr_val = VOL_W'(env_q[ENV_W-1:0]);
        dr_next = w_vol <= dr_val ? '0 : w_vol - dr_val;
    end

    logic [VOL_W-1:0] vol_next;
    logic [1:0] adsr_next;
    always_comb begin
        vol_next = w_vol;
        adsr_next = ch_adsr[ch];
        case (ch_adsr[ch])
            ADSR_ATTACK: begin
                vol_next = w_vol + VOL_W'(env_q[ENV_W-1:0]);
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

    /* Unreset: a reset here would make these flops again. */
    always_ff @(posedge clk) begin
        if (state == P_STEP) begin
            ch_sample[w_ch] <= wave;
            ch_vol[w_ch] <= vol_next;
            ch_phase[w_ch] <= w_phase;
            ch_noise1[w_ch] <= noise_step ? noise1_x : w_noise1;
            ch_noise2[w_ch] <= noise_step ? w_noise2 + noise1_x : w_noise2;
        end
        if (snoop_cfg) begin
            case (snoop_off[2:0])
                3'd0: cfg_freq_lo[snoop_ch] <= q_val;
                3'd1: cfg_freq_hi[snoop_ch] <= q_val;
                3'd2: cfg_duty[snoop_ch] <= q_val;
                3'd3: cfg_va[snoop_ch] <= q_val;
                3'd4: cfg_vd[snoop_ch] <= q_val;
                3'd5: cfg_wr[snoop_ch] <= q_val;
                3'd6: cfg_pan[snoop_ch] <= q_val;
                default: ;  /* the structure's padding */
            endcase
        end
    end

    initial begin
        xaddr = 16'hFFFF;
        xreg_pend = 1'b0;
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
        pan = '0;
        mix_l = MIX_ROUND;
        mix_r = MIX_ROUND;
        mix_i = '0;
        mix_s = '0;
        w_inc = '0;
        sine_q = '0;
        env_q = '0;
        psg_l = '0;
        psg_r = '0;
        psg_valid = 1'b0;
        psg_tick = 1'b0;
    end
    always_ff @(posedge clk) begin
        psg_valid <= 1'b0;
        psg_tick <= tickctr == 13'(TICKS_PER_SAMPLE - 1);

        if (tickctr == 13'(TICKS_PER_SAMPLE - 1))
            tickctr <= '0;
        else
            tickctr <= tickctr + 13'd1;

        /* A walk that outlasts its tick drops the sample silently. */
        if (tickctr == 13'd0 && state != P_IDLE)
            $fatal(1, "psg walk overrun");

        case (state)
            P_IDLE: begin
                if (xreg_pend) begin
                    /* The phase persists across an xreg; the envelopes
                     * and noise go with clr. */
                    xreg_pend <= 1'b0;
                    clr <= 1'b1;
                    for (int i = 0; i < 8; i++)
                        ch_adsr[i] <= ADSR_RELEASE;
                end
                if (tickctr == 13'd0) begin
                    /* Always runs: the bell does not wait on a program. */
                    ch <= '0;
                    mix_l <= MIX_ROUND;
                    mix_r <= MIX_ROUND;
                    mix_i <= '0;
                    state <= P_MIX;
                end
            end
            P_MIX: begin
                case (mix_i)
                    2'd0: begin
                        mix_s <= 17'((mul_p + 31'sd2048) >>> 12);
                        pan <= pan_c;
                    end
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
                psg_l <= clamped(21'(mix_l >>> 7));
                psg_r <= clamped(21'(mix_r >>> 7));
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
                w_inc <= 32'(inc_prod[56:PHASE_SHIFT]);
                state <= P_PH;
            end
            P_PH: begin
                /* A clock of its own, so the wave reads a standing phase
                 * rather than a 32-bit add. */
                w_phase <= ph_next;
                sine_q <= aud_rom[{1'b0, ph_next[31:24]}];
                env_q <= aud_rom[env_addr];
                state <= P_STEP;
            end
            P_STEP: begin
                ch_adsr[ch] <= adsr_next;
                ch <= ch + 4'd1;
                if (ch == 4'd8) begin
                    clr <= 1'b0;
                    psg_valid <= 1'b1;
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

        /* The ninth voice's gate is a level in a register, so its edge is
         * taken here. Holding it high must not restrike a dead note. */
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
            xaddr <= xaddr_wdata;
            xreg_pend <= 1'b1;
        end
        if (gate_any_we)
            gate_any <= gate_any_wdata;
    end

    function automatic logic signed [15:0] clamped(logic signed [20:0] s);
        if (s < -21'sd32768)
            s = -21'sd32768;
        if (s > 21'sd32767)
            s = 21'sd32767;
        return 16'(s);
    endfunction

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_psg;
    always_comb unused_psg = ^{cf[63:56], bel_wdata[31:24],
                                   inc_prod[PHASE_SHIFT-1:0],
                                   sine_q[ROM_W-1:16], env_q[ROM_W-1:ENV_W]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
