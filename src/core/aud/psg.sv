/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Nine voices, bit-exact with core/aud/psg.c: the same arithmetic,
 * accumulator widths and wrapping. That agreement is what fixes every
 * width below. The walk mixes and steps each voice in turn where psg.c
 * mixes all eight and then steps all eight, which comes to the same
 * thing because no voice reads another's state.
 *
 * Eight voices are the program's. Their registers arrive as the 6502's
 * writes and are held here, so the engine never reads XRAM, and a pointer
 * write starts them at zero. The ninth voice is the bell, which has to
 * ring whether or not a program installed a pointer.
 *
 * A multiplier for the envelope, one a side for the mix, one for the
 * phase, one adder and one comparator for the envelope, and the voice
 * state in memories the cursor addresses. The constants, the pan
 * weights among them, are memories too, read a clock ahead of the step
 * that spends them: a table indexed by a register is a mux, and these
 * were the largest logic in the module before they were a read.
 *
 * psg_tick is the sample divider, not the end of a walk.
 *
 * A savestate reads and writes the voices through the sst port at
 * savestate offsets: the structures at 0x640, the bell's at 0x6D0, the
 * voice words at 0x700 and the flops at 0x788. Both memories run on
 * clk_sys, the flops on clk. Between walks every other flop and every
 * registered read is dead, so only that state is kept.
 */

module psg
    import psg_pkg::*;
#(
    /* The arithmetic's rate. Separate from the tick, which only says how
     * often a sample happens: the lockstep shortens that, not this. */
    parameter int RATE = 48000,
    /* 50.4 MHz / 48,000 exactly. */
    parameter int TICKS_PER_SAMPLE = 1050
) (
    input logic clk,
    input logic clk_sys,

    /* The device register: the sw's validated pointer, 0xFFFF off. */
    input logic xaddr_we,
    input logic [15:0] xaddr_wdata,

    /* Every write the 6502 makes to XRAM through RW0 or RW1, the only
     * writes that reach the engine on any machine. */
    input logic q_we,
    input logic [15:0] q_addr,
    input logic [7:0] q_val,

    /* One byte of the bell's channel structure. Byte 6 strikes or
     * releases its gate, as a 6502 write does a voice's. */
    input logic bel_we,
    input logic [2:0] bel_idx,
    input logic [7:0] bel_val,

    /* Signed at full scale; the platform narrows. */
    output logic signed [15:0] psg_l,
    output logic signed [15:0] psg_r,
    output logic psg_valid,

    output logic psg_tick,

    /* High from a pointer write until the eight voices' registers are
     * zero. A write to the block before then may be lost. */
    output logic psg_busy,

    /* sst_own gives the memories' ports to sst_addr, written on sst_we.
     * sst_st_we writes the flop word at sst_addr on clk. sst_park holds
     * the sample counter, so no walk starts and psg_tick stays low. */
    input logic sst_own,
    input logic [10:0] sst_addr,
    input logic sst_we,
    input logic sst_st_we,
    input logic [31:0] sst_wdata,
    input logic sst_park,
    output logic [31:0] psg_sst_rdata,
    /* Idle, with no walk due for QUIET_LEAD clocks. */
    output logic psg_quiet
);

    /* Word k of voice v at 0x700 + 16k + v, k = 0 to 4 in the order
     * sample, volume, phase, noise1, noise2. The memory is addressed the
     * same way, so the offset is the address. */
    localparam logic [2:0] K_SMP = 3'd0;
    localparam logic [2:0] K_VOL = 3'd1;
    localparam logic [2:0] K_PH = 3'd2;
    localparam logic [2:0] K_N1 = 3'd3;
    localparam logic [2:0] K_N2 = 3'd4;
    logic sel_cfg, sel_bel, sel_v, sel_st;
    logic [2:0] st_k;
    always_comb begin
        sel_cfg = sst_addr[10:6] == 5'h19;
        sel_bel = sst_addr[10:3] == 8'hDA;
        sel_v = sst_addr[10:7] == 4'hE && sst_addr[6:4] <= K_N2
            && sst_addr[3:0] < 4'd9;
        sel_st = sst_addr[10:3] == 8'hF1;
        st_k = sst_addr[2:0];
    end

    /* A stop lands within a few clocks of psg_quiet, and no walk may
     * start before it does. */
    localparam int QUIET_LEAD = 24;

    /* The C's entries are all N << 16 with N no wider than nine bits, so
     * nine bits is stored and the zeros go back on at the read. */
    localparam logic [8:0] VOL_TABLE[16] = '{
        9'd256, 9'd204, 9'd168, 9'd142,
        9'd120, 9'd102, 9'd86, 9'd73,
        9'd61, 9'd50, 9'd40, 9'd31,
        9'd22, 9'd14, 9'd7, 9'd0
    };
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

    /* The pointer moves on the write. What it resets is held to the next
     * idle clock that starts no walk; a clear landing mid-walk would tear
     * the sample. */
    logic xreg_pend;

    /* The voice the walk stands on, and its clock within that voice. */
    logic [3:0] ch /*verilator public_flat_rd*/;
    logic [3:0] t /*verilator public_flat_rd*/;
    /* Flops, because the snoop strikes a gate on the clock its write
     * lands, which is a second write port. */
    logic [1:0] ch_adsr[9] /*verilator public_flat_rd*/;

    /* The block may start anywhere, so a write is the structure's when its
     * offset from the start is under sixty-four. */
    logic [15:0] snoop_off;
    always_comb snoop_off = q_addr - xaddr;
    logic [2:0] snoop_ch;
    always_comb snoop_ch = snoop_off[5:3];
    logic snoop_cfg;
    always_comb snoop_cfg = q_we && enabled && snoop_off[15:6] == 10'd0;
    /* A gate is an edge, struck or released on the clock it lands. */
    logic snoop_gate;
    always_comb snoop_gate = snoop_cfg && snoop_off[2:0] == 3'd6;

    /* The channel structures a byte a word, the bell's as row 8. Written
     * and read on one clock with no read-write check waived, so a byte
     * landing on the byte being read returns the old one and the new one
     * plays from the next sample. The structure's eighth byte is padding
     * and is never written.
     *
     * Initialized rather than reset: the walk reads these on the first
     * sample, before any program has written them. */
    (* ramstyle = "M10K" *)
    logic [7:0] cfg[128] /*verilator public_flat_rd*/;
    initial
        for (int i = 0; i < 128; i++)
            cfg[i] = '0;

    /* A pointer write zeroes the eight voices' bytes, byte zc on each
     * clock, in a pass of its own, which neither the sample tick nor the
     * park holds up. It writes eight bytes a voice where a walk reads one a
     * voice every fifteen clocks, so a walk that starts behind it reads
     * zeros. */
    logic zing;
    logic [5:0] zc;
    initial begin
        zing = 1'b0;
        zc = '0;
    end

    /* A bell byte that lands on a snooped byte's clock waits one clock.
     * One is enough: the 6502 writes a PHI2 apart, the soft CPU's strobes
     * are two clocks apart, and the soft CPU is halted before any stop. */
    logic hold_v;
    logic [2:0] hold_idx;
    logic [7:0] hold_val;
    initial begin
        hold_v = 1'b0;
        hold_idx = '0;
        hold_val = '0;
    end

    logic [6:0] sst_cfg_a;
    always_comb sst_cfg_a = sel_bel ? {4'd8, sst_addr[2:0]}
                                    : {1'b0, sst_addr[5:0]};

    logic cw_e;
    logic [6:0] cw_a;
    logic [7:0] cw_d;
    always_comb begin
        if (sst_own) begin
            cw_e = sst_we && (sel_cfg || sel_bel);
            cw_a = sst_cfg_a;
            cw_d = sst_wdata[7:0];
        end else if (zing) begin
            cw_e = 1'b1;
            cw_a = {1'b0, zc};
            cw_d = '0;
        end else if (snoop_cfg) begin
            cw_e = 1'b1;
            cw_a = {1'b0, snoop_ch, snoop_off[2:0]};
            cw_d = q_val;
        end else begin
            cw_e = hold_v || bel_we;
            cw_a = {4'd8, hold_v ? hold_idx : bel_idx};
            cw_d = hold_v ? hold_val : bel_val;
        end
        if (cw_a[2:0] == 3'd7)
            cw_e = 1'b0;
    end
    always_ff @(posedge clk_sys) begin
        if (cw_e)
            cfg[cw_a] <= cw_d;
        if (snoop_cfg && bel_we) begin
            hold_v <= 1'b1;
            hold_idx <= bel_idx;
            hold_val <= bel_val;
        end else if (!snoop_cfg)
            hold_v <= 1'b0;
    end

    /* The walk reads byte t of the voice's structure on its clock t. */
    logic [7:0] cfg_q;
    logic [6:0] cr_a;
    always_comb cr_a = sst_own ? sst_cfg_a : {ch, t[2:0]};
    always_ff @(posedge clk_sys)
        cfg_q <= cfg[cr_a];

    /* The voice words, in a block for the same reason. The bell's noise
     * starts where bel_init starts the C bell's, because nothing clears
     * the ninth voice. */
    (* ramstyle = "M10K" *)
    logic [31:0] vm[128] /*verilator public_flat_rd*/;
    initial begin
        for (int i = 0; i < 128; i++)
            vm[i] = '0;
        vm[{K_N1, 4'd8}] = 32'h2301;
        vm[{K_N2, 4'd8}] = 32'hAB89;
    end

    /* The walk reads word t of the voice on its clock t and writes the
     * five back on clocks 10 to 14, volume first. */
    logic [31:0] vq;
    logic [6:0] vr_a;
    always_comb vr_a = sst_own ? sst_addr[6:0] : {t[2:0], ch};
    always_ff @(posedge clk_sys)
        vq <= vm[vr_a];

    logic signed [15:0] r_smp;
    logic [31:0] w_phase;
    logic [15:0] w_noise1, w_noise2;
    logic [31:0] w_inc;
    logic [VOL_W-1:0] w_vol;

    /* Loaded a byte a clock, so cf_r is whole from clock 8. */
    logic [55:0] cf_r;
    logic clr;

    logic [15:0] cf_freq;
    logic [7:0] cf_duty, cf_va, cf_vd, cf_wr, cf_pan;
    always_comb begin
        cf_freq = cf_r[15:0];
        cf_duty = cf_r[23:16];
        cf_va = cf_r[31:24];
        cf_vd = cf_r[39:32];
        cf_wr = cf_r[47:40];
        cf_pan = cf_r[55:48];
    end

    typedef enum logic [1:0] {
        P_IDLE, P_VOICE, P_OUT
    } state_t;
    state_t state /*verilator public_flat_rd*/;

    logic [12:0] tickctr /*verilator public_flat_rd*/;

    /* Each side's sum is a multiply-accumulate, loaded with the
     * rounding on voice 0. Nine voices at full scale and full weight come
     * to under 2^29. */
    localparam logic signed [29:0] MIX_ROUND = 30'sd2048;
    logic signed [29:0] mix_l, mix_r;
    /* The oracle's int8 division truncates toward zero. */
    logic signed [7:0] pan_c;
    always_comb pan_c = cf_pan[7]
        ? 8'(($signed(cf_pan) + 8'sd1) >>> 1)
        : 8'($signed(cf_pan) >>> 1);
    /* The envelope's product, on clock 8. Thirteen bits of the Q24
     * envelope, not nine: at nine a long release moved the gain in 256
     * visible steps. */
    logic signed [16:0] mix_s;
    logic signed [30:0] env_p;
    always_comb
        env_p = 31'(r_smp) * 31'($signed({1'b0, w_vol[VOL_W-1:12]}));

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

    /* The sine table lives in a block RAM, addressed off the phase adder
     * rather than the register the phase lands in, so the word is ready
     * without an extra pipeline register. It is initialized rather than
     * written, and synthesis turns that into a .mif.
     *
     * The envelope's two rate tables share the block above the sine, attack
     * at 256 and decay-and-release at 272. A voice reads one or the other,
     * never both, so they share the second port.
     *
     * The rate is fetched a clock ahead of the step that uses it, so a gate
     * landing exactly on the fetch changes the ADSR state the step reads but
     * not the address the fetch used. That one step moves at the old rate. */
    localparam int ROM_W = 20;
    localparam int ROM_ATK = 256;
    localparam int ROM_DR = 272;
    /* The pan weights, read on clock 8 at ROM_PAN + 63 - pan for the left
     * and + 63 + pan for the right. The zeros either side of the table are
     * the weights at pan -64, which mutes the voice. */
    localparam int ROM_PAN = 289;
    (* ramstyle = "M10K, no_rw_check" *)
    logic [ROM_W-1:0] aud_rom[512];
    initial begin
        for (int i = 0; i < 512; i++)
            aud_rom[i] = '0;
        for (int i = 0; i < 256; i++)
            aud_rom[i] = ROM_W'(PSG_SINE[i]);
        for (int i = 0; i < 16; i++) begin
            aud_rom[ROM_ATK+i] = ROM_W'(ATTACK_TABLE[i]);
            aud_rom[ROM_DR+i] = ROM_W'(DR_TABLE[i]);
        end
        for (int i = 0; i < 127; i++)
            aud_rom[ROM_PAN+i] = ROM_W'(PSG_PAN[i]);
    end
    logic [8:0] pan_l_a, pan_r_a;
    always_comb begin
        pan_l_a = 9'(ROM_PAN + 63) - 9'(pan_c);
        pan_r_a = 9'(ROM_PAN + 63) + 9'(pan_c);
    end
    /* The weights, a clock after their read. */
    logic signed [11:0] w_l, w_r;
    always_comb begin
        w_l = $signed({1'b0, sine_q[10:0]});
        w_r = $signed({1'b0, env_q[10:0]});
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
            4'd4: wave = past_duty ? -16'sd32767 : $signed(w_noise2);
            default: wave = 16'sd0;
        endcase
    end

    logic noise_step;
    always_comb noise_step = cf_wr[7:4] == 4'd4 && !past_duty;
    logic [15:0] noise1_x;
    always_comb noise1_x = w_noise1 ^ w_noise2;

    logic [VOL_W-1:0] vol_peak, vol_sus;
    always_comb begin
        vol_peak = VOL_W'({VOL_TABLE[cf_va[7:4]], 16'd0});
        vol_sus = VOL_W'({VOL_TABLE[cf_vd[7:4]], 16'd0});
    end
    /* The table only falls, so its entries compare as their indexes do. */
    logic sus_le_peak;
    always_comb sus_le_peak = cf_vd[7:4] >= cf_va[7:4];

    /* Decay and release are the same subtraction against different
     * nibbles, so the nibble is what gets muxed. */
    logic [3:0] dr_idx;
    always_comb dr_idx = ch_adsr[ch] == ADSR_DECAY ? cf_vd[3:0] : cf_wr[3:0];
    logic [8:0] env_addr;
    always_comb env_addr = ch_adsr[ch] == ADSR_ATTACK
        ? 9'(ROM_ATK) + {5'd0, cf_va[3:0]}
        : 9'(ROM_DR) + {5'd0, dr_idx};
    /* One adder for the three steps: attack adds the rate, decay and
     * release subtract it and stop at zero, where the borrow says so. */
    logic [VOL_W-1:0] rate;
    logic env_sub;
    logic [VOL_W:0] env_s;
    logic [VOL_W-1:0] dr_next;
    always_comb begin
        rate = VOL_W'(env_q[ENV_W-1:0]);
        env_sub = ch_adsr[ch] != ADSR_ATTACK;
        env_s = env_sub ? {1'b0, w_vol} - {1'b0, rate}
                        : {1'b0, w_vol} + {1'b0, rate};
        dr_next = env_s[VOL_W] ? '0 : env_s[VOL_W-1:0];
    end
    /* One comparator too: attack's sum against the peak, or the sustain
     * against decay's difference. */
    logic env_ge;
    always_comb env_ge = env_sub ? vol_sus >= dr_next
                                 : env_s[VOL_W-1:0] >= vol_peak;

    logic [VOL_W-1:0] vol_next;
    logic [1:0] adsr_next;
    always_comb begin
        vol_next = w_vol;
        adsr_next = ch_adsr[ch];
        case (ch_adsr[ch])
            ADSR_ATTACK: begin
                vol_next = env_s[VOL_W-1:0];
                if (env_ge) begin
                    vol_next = vol_peak;
                    adsr_next = ADSR_DECAY;
                end
            end
            ADSR_DECAY: begin
                vol_next = dr_next;
                if (env_ge) begin
                    adsr_next = ADSR_SUSTAIN;
                    if (sus_le_peak)
                        vol_next = vol_sus;
                end
            end
            ADSR_SUSTAIN: begin
                if (sus_le_peak)
                    vol_next = vol_sus;
            end
            default: vol_next = dr_next;  /* release */
        endcase
    end

    logic vw_e, step_w;
    logic [2:0] wk;
    logic [6:0] vw_a;
    logic [31:0] w_d, vw_d;
    always_comb begin
        step_w = state == P_VOICE && t >= 4'd10;
        case (t)
            4'd10: wk = K_VOL;
            4'd11: wk = K_SMP;
            4'd12: wk = K_PH;
            4'd13: wk = K_N1;
            default: wk = K_N2;
        endcase
        case (wk)
            K_VOL: w_d = 32'(vol_next);
            K_SMP: w_d = {16'd0, wave};
            K_PH: w_d = w_phase;
            K_N1: w_d = {16'd0, noise_step ? noise1_x : w_noise1};
            default: w_d = {16'd0, noise_step ? w_noise2 + noise1_x
                                              : w_noise2};
        endcase
        vw_e = sst_own ? sst_we && sel_v : step_w;
        vw_a = sst_own ? sst_addr[6:0] : {wk, ch};
        vw_d = sst_own ? sst_wdata : w_d;
    end
    always_ff @(posedge clk_sys)
        if (vw_e)
            vm[vw_a] <= vw_d;

    initial begin
        xaddr = 16'hFFFF;
        xreg_pend = 1'b0;
        for (int i = 0; i < 9; i++)
            ch_adsr[i] = ADSR_RELEASE;
        clr = 1'b0;
        r_smp = '0;
        w_phase = '0;
        w_noise1 = '0;
        w_noise2 = '0;
        w_vol = '0;
        cf_r = '0;
        ch = '0;
        t = '0;
        state = P_IDLE;
        tickctr = '0;
        mix_l = MIX_ROUND;
        mix_r = MIX_ROUND;
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
        psg_tick <= tickctr == 13'(TICKS_PER_SAMPLE - 1) && !sst_park;

        if (sst_park)
            ;
        else if (tickctr == 13'(TICKS_PER_SAMPLE - 1))
            tickctr <= '0;
        else
            tickctr <= tickctr + 13'd1;

        /* A walk that outlasts its tick drops the sample silently. */
        if (tickctr == 13'd0 && !sst_park && state != P_IDLE)
            $fatal(1, "psg walk overrun");

        if (zing) begin
            zc <= zc + 6'd1;
            if (zc == 6'd63)
                zing <= 1'b0;
        end

        case (state)
            P_IDLE: begin
                if (xreg_pend && tickctr != 13'd0) begin
                    /* The envelopes, the noise and the phase go with
                     * clr and the registers with the pass, as psg.c's
                     * psg_map resets them. */
                    xreg_pend <= 1'b0;
                    clr <= 1'b1;
                    zing <= 1'b1;
                    zc <= '0;
                    for (int i = 0; i < 8; i++)
                        ch_adsr[i] <= ADSR_RELEASE;
                end
                if (tickctr == 13'd0 && !sst_park) begin
                    /* Always runs: the bell does not wait on a program. */
                    ch <= '0;
                    t <= '0;
                    state <= P_VOICE;
                end
            end
            P_VOICE: begin
                /* The reads land a clock after their address. */
                if (t >= 4'd1 && t <= 4'd7)
                    cf_r <= {cfg_q, cf_r[55:8]};
                case (t)
                    4'd1: r_smp <= $signed(vq[15:0]);
                    4'd2: w_vol <= clr && !ch[3] ? '0 : vq[VOL_W-1:0];
                    4'd3: w_phase <= clr && !ch[3] ? '0 : vq;
                    4'd4: w_noise1 <= clr && !ch[3] ? 16'h2301 : vq[15:0];
                    4'd5: w_noise2 <= clr && !ch[3] ? 16'hAB89 : vq[15:0];
                    4'd8: begin
                        mix_s <= 17'((env_p + 31'sd2048) >>> 12);
                        w_inc <= 32'(inc_prod[56:PHASE_SHIFT]);
                        sine_q <= aud_rom[pan_l_a];
                        env_q <= aud_rom[pan_r_a];
                    end
                    4'd9: begin
                        mix_l <= (ch == 4'd0 ? MIX_ROUND : mix_l)
                            + 30'(mix_s) * 30'(w_l);
                        mix_r <= (ch == 4'd0 ? MIX_ROUND : mix_r)
                            + 30'(mix_s) * 30'(w_r);
                        /* A clock of its own, so the wave reads a standing
                         * phase rather than a 32-bit add. */
                        w_phase <= ph_next;
                        sine_q <= aud_rom[{1'b0, ph_next[31:24]}];
                        env_q <= aud_rom[env_addr];
                    end
                    4'd10: ch_adsr[ch] <= adsr_next;
                    default: ;
                endcase
                t <= t + 4'd1;
                if (t == 4'd14) begin
                    t <= '0;
                    ch <= ch + 4'd1;
                    if (ch == 4'd8)
                        state <= P_OUT;
                end
            end
            P_OUT: begin
                psg_l <= clamped(21'(mix_l >>> 12));
                psg_r <= clamped(21'(mix_r >>> 12));
                psg_valid <= 1'b1;
                clr <= 1'b0;
                ch <= '0;
                state <= P_IDLE;
            end
            default: state <= P_IDLE;
        endcase

        /* After the case, so a write landing on the same step as the state
         * machine wins it. */
        if (snoop_gate) begin
            if (!q_val[0] && ch_adsr[{1'b0, snoop_ch}] != ADSR_RELEASE)
                ch_adsr[{1'b0, snoop_ch}] <= ADSR_RELEASE;
            if (q_val[0] && ch_adsr[{1'b0, snoop_ch}] == ADSR_RELEASE)
                ch_adsr[{1'b0, snoop_ch}] <= ADSR_ATTACK;
        end
        if (bel_we && bel_idx == 3'd6) begin
            if (!bel_val[0] && ch_adsr[8] != ADSR_RELEASE)
                ch_adsr[8] <= ADSR_RELEASE;
            if (bel_val[0] && ch_adsr[8] == ADSR_RELEASE)
                ch_adsr[8] <= ADSR_ATTACK;
        end

        /* After the case, so a write on an apply clock is kept for
         * the next boundary instead of vanishing under the clear. */
        if (xaddr_we) begin
            xaddr <= xaddr_wdata;
            xreg_pend <= 1'b1;
        end

        if (sst_st_we && sel_st)
            case (st_k)
                3'd1: clr <= sst_wdata[25];
                3'd2: begin
                    xaddr <= sst_wdata[15:0];
                    for (int i = 0; i < 8; i++)
                        ch_adsr[i] <= sst_wdata[16 + 2 * i+:2];
                end
                3'd3: {psg_r, psg_l} <= sst_wdata;
                3'd4: {ch_adsr[8], tickctr} <= sst_wdata[14:0];
                default: ;
            endcase
    end

    /* The soft CPU waits on this after a pointer write, so no bell byte
     * comes while the pass has the registers' port. */
    always_comb psg_busy = xreg_pend || zing;

    always_comb
        psg_quiet = state == P_IDLE && !xreg_pend && !zing && tickctr != 13'd0
            && tickctr < 13'(TICKS_PER_SAMPLE - 1 - QUIET_LEAD);

    /* Word 0x788 is empty, and clr is bit 25 of 0x789. */
    logic [31:0] st_rd;
    always_comb begin
        case (st_k)
            3'd1: st_rd = {6'd0, clr, 25'd0};
            3'd2: st_rd = {ch_adsr[7], ch_adsr[6], ch_adsr[5], ch_adsr[4],
                           ch_adsr[3], ch_adsr[2], ch_adsr[1], ch_adsr[0],
                           xaddr};
            3'd3: st_rd = {psg_r, psg_l};
            3'd4: st_rd = {17'd0, ch_adsr[8], tickctr};
            default: st_rd = '0;
        endcase
        psg_sst_rdata = sel_cfg || sel_bel ? {24'd0, cfg_q}
            : sel_v ? vq
            : sel_st ? st_rd : '0;
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
    always_comb unused_psg = ^{inc_prod[PHASE_SHIFT-1:0],
                               sine_q[ROM_W-1:16], env_q[ROM_W-1:ENV_W]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
