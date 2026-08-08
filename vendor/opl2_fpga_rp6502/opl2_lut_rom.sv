// rp6502: the log-sine and exp tables share one block.
//
// The vendored core carries these as two generated case-statement
// modules, 256x12 and 256x10, and Quartus gives each its own M10K for a
// quarter of the bits one block holds. The phase generator reads them
// at different pipeline stages — the exp address is computed from the
// log-sine output — so one 512x12 store with two synchronous read
// ports carries both: log-sine in the low half, exp in the high. Two
// reads of one array is a dual-port ROM, one block at this width.
//
// The contents come from opl2_lut_gen.py, which parses the vendor case
// arms and recomputes the documented formulas as a tripwire, so the
// words here are the submodule's by construction. The read is
// registered with no enable and no reset, exactly like the modules it
// replaces.

module opl2_lut_rom
    import opl2_lut_pkg::*;
(
    input wire clk,
    input wire [7:0] theta,
    output logic [11:0] log_sin_out = 0,
    input wire [7:0] exp_in,
    output logic [9:0] exp_out = 0
);

    logic [11:0] rom[512];
    initial
        for (int i = 0; i < 512; i++)
            rom[i] = OPL2_LUT[i];

    always_ff @(posedge clk) begin
        log_sin_out <= rom[{1'b0, theta}];
        exp_out <= rom[{1'b1, exp_in}][9:0];
    end

endmodule
