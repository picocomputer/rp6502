/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * M6522 VIA, per-cycle identical to the emulator's chips/m6522.h as the
 * machine wires it: register file, both timers with their delay pipelines,
 * IFR/IER with the one-cycle IRQ pipeline, and the T1 PB7 toggle. The ports
 * are unwired here as on the board — inputs read low, CA/CB never see an
 * edge — so the port-input machinery reduces to constants and is documented
 * out rather than carried dead.
 *
 * Every field operation and its ordering comes from the C: register access
 * effects land first, the timers tick on the pipeline bits fed two cycles
 * before, the IRQ flag follows its own one-cycle pipe, and the pipelines
 * shift last. One quirk is kept deliberately: in PB6 count mode the C
 * compares the fresh port inputs (all zero) against the pins it drove last
 * cycle, so a driven-high PB6 reads as a falling edge every cycle.
 *
 * Read data is combinational from pre-tick state; read side effects land at
 * the clock edge with everything else.
 */

module via (
    input logic clk,
    input logic rst_n,
    input logic en,

    input logic cs,
    input logic we,
    input logic [3:0] rs,
    input logic [7:0] data_i,

    output logic [7:0] via_data,
    output logic via_irq
);

    // Register indices
    localparam logic [3:0] REG_RB = 4'h0;
    localparam logic [3:0] REG_RA = 4'h1;
    localparam logic [3:0] REG_DDRB = 4'h2;
    localparam logic [3:0] REG_DDRA = 4'h3;
    localparam logic [3:0] REG_T1CL = 4'h4;
    localparam logic [3:0] REG_T1CH = 4'h5;
    localparam logic [3:0] REG_T1LL = 4'h6;
    localparam logic [3:0] REG_T1LH = 4'h7;
    localparam logic [3:0] REG_T2CL = 4'h8;
    localparam logic [3:0] REG_T2CH = 4'h9;
    localparam logic [3:0] REG_SR = 4'hA;
    localparam logic [3:0] REG_ACR = 4'hB;
    localparam logic [3:0] REG_PCR = 4'hC;
    localparam logic [3:0] REG_IFR = 4'hD;
    localparam logic [3:0] REG_IER = 4'hE;
    localparam logic [3:0] REG_RA_NOH = 4'hF;

    // IFR bits
    localparam int IRQ_T2 = 5;
    localparam int IRQ_T1 = 6;
    localparam int IRQ_ANY = 7;

    logic [7:0] outr_a /*verilator public_flat_rw*/;
    logic [7:0] outr_b /*verilator public_flat_rw*/;
    logic [7:0] ddr_a /*verilator public_flat_rw*/;
    logic [7:0] ddr_b /*verilator public_flat_rw*/;
    logic [7:0] pins_a /*verilator public_flat_rw*/;
    logic [7:0] pins_b /*verilator public_flat_rw*/;
    logic [15:0] t1_latch /*verilator public_flat_rw*/;
    logic [15:0] t1_counter /*verilator public_flat_rw*/;
    logic t1_tbit /*verilator public_flat_rw*/;
    logic [15:0] t1_pip /*verilator public_flat_rw*/;
    logic [15:0] t2_latch /*verilator public_flat_rw*/;
    logic [15:0] t2_counter /*verilator public_flat_rw*/;
    logic t2_tbit /*verilator public_flat_rw*/;
    logic [15:0] t2_pip /*verilator public_flat_rw*/;
    logic [7:0] acr /*verilator public_flat_rw*/;
    logic [7:0] pcr /*verilator public_flat_rw*/;
    logic [7:0] ifr /*verilator public_flat_rw*/;
    logic [7:0] ier /*verilator public_flat_rw*/;
    logic [15:0] int_pip /*verilator public_flat_rw*/;

    // ------------------------------------------------------------------
    // Combinational read, from pre-tick state
    // ------------------------------------------------------------------

    always_comb begin
        via_data = 8'h00;
        if (cs && !we) begin
            case (rs)
                // With port latching on, reads return the input latch, which
                // never captures anything on unwired ports.
                REG_RB: via_data = acr[1] ? 8'h00 : pins_b;
                REG_RA, REG_RA_NOH: via_data = acr[0] ? 8'h00 : pins_a;
                REG_DDRB: via_data = ddr_b;
                REG_DDRA: via_data = ddr_a;
                REG_T1CL: via_data = t1_counter[7:0];
                REG_T1CH: via_data = t1_counter[15:8];
                REG_T1LL: via_data = t1_latch[7:0];
                REG_T1LH: via_data = t1_latch[15:8];
                REG_T2CL: via_data = t2_counter[7:0];
                REG_T2CH: via_data = t2_counter[15:8];
                REG_SR: via_data = 8'h00;
                REG_ACR: via_data = acr;
                REG_PCR: via_data = pcr;
                REG_IFR: via_data = ifr;
                REG_IER: via_data = ier | 8'h80;
                default: via_data = 8'h00;
            endcase
        end
    end

    // ------------------------------------------------------------------
    // Tick: access effects, then timers, then IRQ, then pipelines
    // ------------------------------------------------------------------

    logic [7:0] n_outr_a, n_outr_b, n_ddr_a, n_ddr_b;
    logic [15:0] n_t1_latch, n_t1_counter, n_t2_latch, n_t2_counter;
    logic n_t1_tbit, n_t2_tbit;
    logic [15:0] n_t1_pip, n_t2_pip, n_int_pip;
    logic [7:0] n_acr, n_pcr, n_ifr, n_ier;
    logic [7:0] n_pins_a, n_pins_b;
    logic t1_out, t2_out;

    // The C's _m6522_clear_intr: dropping the last enabled source also drops
    // the master bit and anything in the IRQ delay pipe.
    function automatic logic [23:0] clear_intr(input logic [7:0] fr,
                                               input logic [7:0] er,
                                               input logic [15:0] pip,
                                               input logic [7:0] mask);
        logic [7:0] f;
        logic [15:0] q;
        f = fr & ~mask;
        q = pip;
        if ((f & er & 8'h7F) == 8'h00) begin
            f = f & 8'h7F;
            q = q & ~16'h00FF;
        end
        return {f, q};
    endfunction

    logic [23:0] ci;

    always_comb begin
        n_outr_a = outr_a;
        n_outr_b = outr_b;
        n_ddr_a = ddr_a;
        n_ddr_b = ddr_b;
        n_t1_latch = t1_latch;
        n_t1_counter = t1_counter;
        n_t1_tbit = t1_tbit;
        n_t1_pip = t1_pip;
        n_t2_latch = t2_latch;
        n_t2_counter = t2_counter;
        n_t2_tbit = t2_tbit;
        n_t2_pip = t2_pip;
        n_acr = acr;
        n_pcr = pcr;
        n_ifr = ifr;
        n_ier = ier;
        n_int_pip = int_pip;
        t1_out = 1'b0;
        t2_out = 1'b0;
        ci = 24'h0;

        // ---- register access effects ----
        if (cs) begin
            if (we) begin
                case (rs)
                    REG_RB: n_outr_b = data_i;
                    REG_RA, REG_RA_NOH: n_outr_a = data_i;
                    REG_DDRB: n_ddr_b = data_i;
                    REG_DDRA: n_ddr_a = data_i;
                    REG_T1CL, REG_T1LL:
                        n_t1_latch = {t1_latch[15:8], data_i};
                    REG_T1CH: begin
                        n_t1_latch = {data_i, t1_latch[7:0]};
                        ci = clear_intr(n_ifr, n_ier, n_int_pip, 8'h01 << IRQ_T1);
                        {n_ifr, n_int_pip} = ci;
                        n_t1_tbit = 1'b0;
                        n_t1_counter = n_t1_latch;
                    end
                    REG_T1LH: begin
                        n_t1_latch = {data_i, t1_latch[7:0]};
                        ci = clear_intr(n_ifr, n_ier, n_int_pip, 8'h01 << IRQ_T1);
                        {n_ifr, n_int_pip} = ci;
                    end
                    REG_T2CL: n_t2_latch = {t2_latch[15:8], data_i};
                    REG_T2CH: begin
                        n_t2_latch = {data_i, t2_latch[7:0]};
                        ci = clear_intr(n_ifr, n_ier, n_int_pip, 8'h01 << IRQ_T2);
                        {n_ifr, n_int_pip} = ci;
                        n_t2_tbit = 1'b0;
                        n_t2_counter = n_t2_latch;
                    end
                    REG_SR: ;  // unimplemented upstream too
                    REG_ACR: begin
                        n_acr = data_i;
                        if (!data_i[5])
                            n_t2_pip[0] = 1'b0;
                    end
                    REG_PCR: n_pcr = data_i;
                    REG_IFR: begin
                        ci = clear_intr(n_ifr, n_ier, n_int_pip,
                                        data_i[7] ? 8'h7F : data_i);
                        {n_ifr, n_int_pip} = ci;
                    end
                    REG_IER: begin
                        if (data_i[7])
                            n_ier = ier | (data_i & 8'h7F);
                        else
                            n_ier = ier & ~(data_i & 8'h7F);
                    end
                    default: ;
                endcase
            end else begin
                case (rs)
                    REG_T1CL: begin
                        ci = clear_intr(n_ifr, n_ier, n_int_pip, 8'h01 << IRQ_T1);
                        {n_ifr, n_int_pip} = ci;
                    end
                    REG_T2CL: begin
                        ci = clear_intr(n_ifr, n_ier, n_int_pip, 8'h01 << IRQ_T2);
                        {n_ifr, n_int_pip} = ci;
                    end
                    // RB/RA clear the CA/CB flags, which never set here.
                    default: ;
                endcase
            end
        end

        // ---- T1 ----
        if (n_t1_pip[0])
            n_t1_counter = n_t1_counter - 16'd1;
        t1_out = n_t1_counter == 16'hFFFF;
        if (t1_out) begin
            if (n_acr[6]) begin
                n_t1_tbit = !n_t1_tbit;
                n_ifr[IRQ_T1] = 1'b1;
            end else if (!n_t1_tbit) begin
                n_ifr[IRQ_T1] = 1'b1;
                n_t1_tbit = 1'b1;
            end
            n_t1_pip[9] = 1'b1;
        end
        if (n_t1_pip[8])
            n_t1_counter = n_t1_latch;

        // ---- T2 ----
        if (n_acr[5]) begin
            // The C compares fresh inputs (zero here) with the pins it drove
            // last cycle, so PB6 driven high counts every cycle.
            if (pins_b[6])
                n_t2_counter = n_t2_counter - 16'd1;
        end else if (n_t2_pip[0]) begin
            n_t2_counter = n_t2_counter - 16'd1;
        end
        t2_out = n_t2_counter == 16'hFFFF;
        if (t2_out && !n_t2_tbit) begin
            n_ifr[IRQ_T2] = 1'b1;
            n_t2_tbit = 1'b1;
        end

        // ---- IRQ, one cycle behind its sources ----
        if (n_int_pip[0])
            n_ifr[IRQ_ANY] = 1'b1;

        // ---- ports: output drivers only, inputs read low ----
        n_pins_a = n_outr_a & n_ddr_a;
        n_pins_b = n_outr_b & n_ddr_b;
        if (n_acr[7])
            n_pins_b[7] = n_t1_tbit;

        // ---- pipelines shift last ----
        n_t1_pip = ((n_t1_pip | 16'h0004) >> 1) & 16'h7F7F;
        n_t2_pip = ((n_t2_pip | 16'h0004) >> 1) & 16'h7F7F;
        if ((n_ifr & n_ier & 8'h7F) != 8'h00)
            n_int_pip[1] = 1'b1;
        n_int_pip = (n_int_pip >> 1) & 16'h7F7F;
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            outr_a <= 8'h00;
            outr_b <= 8'h00;
            ddr_a <= 8'h00;
            ddr_b <= 8'h00;
            pins_a <= 8'h00;
            pins_b <= 8'h00;
            t1_latch <= 16'hFFFF;
            t1_counter <= 16'h0000;
            t1_tbit <= 1'b0;
            t1_pip <= 16'h0000;
            t2_latch <= 16'hFFFF;
            t2_counter <= 16'h0000;
            t2_tbit <= 1'b0;
            t2_pip <= 16'h0000;
            acr <= 8'h00;
            pcr <= 8'h00;
            ifr <= 8'h00;
            ier <= 8'h00;
            int_pip <= 16'h0000;
            via_irq <= 1'b0;
        end else if (en) begin
            outr_a <= n_outr_a;
            outr_b <= n_outr_b;
            ddr_a <= n_ddr_a;
            ddr_b <= n_ddr_b;
            pins_a <= n_pins_a;
            pins_b <= n_pins_b;
            t1_latch <= n_t1_latch;
            t1_counter <= n_t1_counter;
            t1_tbit <= n_t1_tbit;
            t1_pip <= n_t1_pip;
            t2_latch <= n_t2_latch;
            t2_counter <= n_t2_counter;
            t2_tbit <= n_t2_tbit;
            t2_pip <= n_t2_pip;
            acr <= n_acr;
            pcr <= n_pcr;
            ifr <= n_ifr;
            ier <= n_ier;
            int_pip <= n_int_pip;
            via_irq <= n_ifr[IRQ_ANY];
        end
    end

endmodule
