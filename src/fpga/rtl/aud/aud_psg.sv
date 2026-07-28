/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The PSG of ria/aud/psg.c, one sample every four thousand two hundred
 * clocks: eight channels of sine, square, sawtooth, triangle and noise
 * through the 6581's envelope rates. Each sample walks the handler's
 * exact order — the previous sample mixes out under the fresh pan while
 * the phase, wave, and envelope advance behind it, and the gate queue
 * drains last, at most thirty-two entries. The phase increment divides
 * 2^32 * freq by 72000 through a restoring divider because the oracle
 * divides, and every accumulator wraps at the C's own width. Gates
 * arrive from the RW engine's write snoop; the queue drops new entries
 * when full, like the firmware's ring. The bell rides the same grid the
 * way the C rides the handler: struck from the console, stepped once
 * per sample, mixed under the channels after their shift — and when the
 * pointer is parked the walk still runs, the standing bell alone, as
 * aud_init leaves the firmware.
 */

module aud_psg
    import aud_sine_pkg::*;
#(
    parameter int TICKS_PER_SAMPLE = 4200
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

    /* The console's BEL scan: one teletype bell per pulse. */
    input logic bel_strike,

    /* One stereo sample per tick, PWM levels centered at 512. */
    output logic [9:0] aud_psg_l,
    output logic [9:0] aud_psg_r,
    output logic aud_psg_valid
);

    localparam logic [31:0] VOL_TABLE[16] = '{
        32'd256 << 16, 32'd204 << 16, 32'd168 << 16, 32'd142 << 16,
        32'd120 << 16, 32'd102 << 16, 32'd86 << 16, 32'd73 << 16,
        32'd61 << 16, 32'd50 << 16, 32'd40 << 16, 32'd31 << 16,
        32'd22 << 16, 32'd14 << 16, 32'd7 << 16, 32'd0
    };
    localparam logic [31:0] ATTACK_TABLE[16] = '{
        32'd16777216 / (24 * 2), 32'd16777216 / (24 * 8),
        32'd16777216 / (24 * 16), 32'd16777216 / (24 * 24),
        32'd16777216 / (24 * 38), 32'd16777216 / (24 * 56),
        32'd16777216 / (24 * 68), 32'd16777216 / (24 * 80),
        32'd16777216 / (24 * 100), 32'd16777216 / (24 * 250),
        32'd16777216 / (24 * 500), 32'd16777216 / (24 * 800),
        32'd16777216 / (24 * 1000), 32'd16777216 / (24 * 3000),
        32'd16777216 / (24 * 5000), 32'd16777216 / (24 * 8000)
    };
    localparam logic [31:0] DR_TABLE[16] = '{
        32'd16777216 / (24 * 6), 32'd16777216 / (24 * 24),
        32'd16777216 / (24 * 48), 32'd16777216 / (24 * 72),
        32'd16777216 / (24 * 114), 32'd16777216 / (24 * 168),
        32'd16777216 / (24 * 204), 32'd16777216 / (24 * 240),
        32'd16777216 / (24 * 300), 32'd16777216 / (24 * 750),
        32'd16777216 / (24 * 1500), 32'd16777216 / (24 * 2400),
        32'd16777216 / (24 * 3000), 32'd16777216 / (24 * 9000),
        32'd16777216 / (24 * 15000), 32'd16777216 / (24 * 24000)
    };

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
    logic signed [7:0] ch_sample[8];
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

    /* The gate queue: the firmware's 256-entry ring, drop when full. */
    /* Read where it is used, so it wants LUT RAM: a block RAM
     * cannot answer without a clock and a register file this
     * wide does not fit. */
    (* ramstyle = "MLAB, no_rw_check" *)
    logic [15:0] queue[256];
    logic [7:0] q_head, q_tail;
    always_ff @(posedge clk) begin
        if (q_we && enabled && q_addr[15:8] == xaddr[15:8]
            && 8'(q_head + 8'd1) != q_tail)
            queue[8'(q_head + 8'd1)] <= {q_addr[7:0], q_val};
    end

    typedef enum logic [2:0] {
        P_IDLE, P_FETCH, P_MIX, P_BEL, P_OUT, P_DIV, P_STEP, P_DRAIN
    } state_t;
    state_t state;

    logic bel_step;
    always_comb bel_step = state == P_BEL;
    logic signed [15:0] bel_out;
    aud_bel bel (
        .clk(clk),
        .rst_n(rst_n),
        .strike(bel_strike),
        .step(bel_step),
        .aud_bel_out(bel_out)
    );

    logic [12:0] tickctr;
    logic [4:0] fw_i, fw_c, fw_n;
    always_comb fw_n = walk_xaddr[1] ? 5'd17 : 5'd16;
    logic gnt_d;

    /* The output mix accumulates in the C's int16. */
    logic signed [15:0] mix_l, mix_r;
    /* The oracle's int8 division truncates toward zero. */
    logic signed [7:0] pan;
    always_comb pan = cf_pan[7]
        ? 8'(($signed(cf_pan) + 8'sd1) >>> 1)
        : 8'($signed(cf_pan) >>> 1);
    logic signed [16:0] mix_s;
    always_comb mix_s =
        17'(($signed(ch_sample[ch]) * $signed({1'b0, ch_vol[ch][24:16]}))
            >>> 8);

    /* One restoring divider makes the phase increment: the 48-bit
     * dividend freq * 2^32 over 72000. */
    logic [47:0] div_q;
    logic [17:0] div_rem;
    logic [5:0] div_i;
    logic [17:0] div_t;
    logic div_ge;
    always_comb begin
        div_t = {div_rem[16:0], div_q[47]};
        div_ge = div_t >= 18'd72000;
    end

    logic [31:0] phase_next;
    always_comb phase_next = ch_phase[ch] + div_q[31:0];
    logic [7:0] ph;
    always_comb ph = phase_next[31:24];
    logic [7:0] duty_half;
    always_comb duty_half = cf_duty >> 1;

    /* The wave sample from the advanced phase. */
    logic signed [7:0] wave;
    always_comb begin
        case (cf_wr[7:4])
            4'd0: begin
                if (ph < 8'd128 - duty_half || ph >= 8'd128 + duty_half)
                    wave = -8'sd127;
                else
                    wave = $signed(AUD_SINE[ph]);
            end
            4'd1: wave = ph > cf_duty ? -8'sd127 : 8'sd127;
            4'd2: wave = ph > cf_duty ? -8'sd127
                : 8'(8'sd127 - $signed(ph));
            4'd3: begin
                if (ph < 8'd128 - duty_half || ph >= 8'd128 + duty_half)
                    wave = -8'sd127;
                else if (ph >= 8'd128)
                    wave = 8'(8'sd127 - $signed(phase_next[30:23]));
                else
                    wave = 8'($signed(phase_next[30:23]) - 8'sd128);
            end
            4'd4: wave = ph > cf_duty ? -8'sd127
                : $signed(ch_noise2[ch][7:0]);
            default: wave = 8'sd0;
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

    /* Queue drain: the C's uint16 recombination and stride filter. */
    logic [15:0] q_ent;
    always_comb q_ent = queue[8'(q_tail + 8'd1)];
    logic [15:0] q_offset;
    always_comb q_offset = {walk_xaddr[15:8], q_ent[15:8]} - walk_xaddr;
    logic [4:0] drained;

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
            q_head <= '0;
            q_tail <= '0;
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
            drained <= '0;
            aud_psg_l <= 10'd512;
            aud_psg_r <= 10'd512;
            aud_psg_valid <= 1'b0;
        end else begin
            gnt_d <= a_gnt;
            aud_psg_valid <= 1'b0;

            if (q_we && enabled && q_addr[15:8] == xaddr[15:8]
                && 8'(q_head + 8'd1) != q_tail)
                q_head <= q_head + 8'd1;

            if (tickctr == 13'(TICKS_PER_SAMPLE - 1))
                tickctr <= '0;
            else
                tickctr <= tickctr + 13'd1;

            case (state)
                P_IDLE: begin
                    if (xreg_pend) begin
                        /* psg_xreg at the boundary: the pointer lands,
                         * the envelopes and noise reset, the queue
                         * empties; the phase persists. */
                        xreg_pend <= 1'b0;
                        xaddr <= xreg_word;
                        for (int i = 0; i < 8; i++) begin
                            ch_noise1[i] <= 32'h67452301;
                            ch_noise2[i] <= 32'hEFCDAB89;
                            ch_vol[i] <= '0;
                            ch_adsr[i] <= ADSR_RELEASE;
                        end
                        q_tail <= q_head;
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
                            state <= P_BEL;
                    end
                end
                P_FETCH: begin
                    if (a_gnt)
                        fw_i <= fw_i + 5'd1;
                    if (gnt_d) begin
                        gather <= gather | (544'(a_rdata) << {fw_c, 5'd0});
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
                        mix_l <= mix_l
                            + 16'((mix_s * (17'sd63 - 17'(pan))) >>> 7);
                        mix_r <= mix_r
                            + 16'((mix_s * (17'sd63 + 17'(pan))) >>> 7);
                    end
                    ch <= ch + 3'd1;
                    if (ch == 3'd7)
                        state <= P_BEL;
                end
                P_BEL: state <= P_OUT;
                P_OUT: begin
                    if (walk_psg) begin
                        aud_psg_l <= clamped(16'(16'(mix_l <<< 2) + bel_out));
                        aud_psg_r <= clamped(16'(16'(mix_r <<< 2) + bel_out));
                        ch <= '0;
                        div_q <= {cf_freq, 32'd0};
                        div_rem <= '0;
                        div_i <= '0;
                        state <= P_DIV;
                    end else begin
                        /* The standing bell, bel_irq_handler's output. */
                        aud_psg_l <= clamped(bel_out);
                        aud_psg_r <= clamped(bel_out);
                        aud_psg_valid <= 1'b1;
                        state <= P_IDLE;
                    end
                end
                P_DIV: begin
                    div_q <= {div_q[46:0], div_ge};
                    div_rem <= div_ge ? div_t - 18'd72000 : div_t;
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
                        drained <= '0;
                        state <= P_DRAIN;
                    end else begin
                        div_q <= {cview[{3'(ch + 3'd1), 6'd0}+:16], 32'd0};
                        div_rem <= '0;
                        div_i <= '0;
                        state <= P_DIV;
                    end
                end
                P_DRAIN: begin
                    /* The strobe marks the whole walk done — output,
                     * generate, drain — so a write after it lands in
                     * the next sample, the handler's own boundary. */
                    if (q_tail == q_head) begin
                        aud_psg_valid <= 1'b1;
                        state <= P_IDLE;
                    end else begin
                        q_tail <= q_tail + 8'd1;
                        if (q_offset[2:0] == 3'd6
                            && q_offset[15:3] < 13'd8) begin
                            if (!q_ent[0]
                                && ch_adsr[q_offset[5:3]] != ADSR_RELEASE)
                                ch_adsr[q_offset[5:3]] <= ADSR_RELEASE;
                            if (q_ent[0]
                                && ch_adsr[q_offset[5:3]] == ADSR_RELEASE)
                                ch_adsr[q_offset[5:3]] <= ADSR_ATTACK;
                        end
                        drained <= drained + 5'd1;
                        if (drained == 5'd31) begin
                            aud_psg_valid <= 1'b1;
                            state <= P_IDLE;
                        end
                    end
                end
                default: state <= P_IDLE;
            endcase

            /* After the case, so a write on an apply clock is kept for
             * the next boundary instead of vanishing under the clear. */
            if (xaddr_we) begin
                xreg_pend <= 1'b1;
                xreg_word <= xaddr_wdata;
            end
        end
    end

    function automatic logic [9:0] clamped(logic signed [15:0] s);
        if (s < -16'sd512)
            s = -16'sd512;
        if (s > 16'sd511)
            s = 16'sd511;
        return 10'(s + 16'sd512);
    endfunction

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_aud_psg;
    always_comb unused_aud_psg = ^{cf[63:56], gather, q_ent, cview,
                                   div_rem[17]};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
