// rp6502: the log-sine table (256x12) and the exp table (256x10) share one
// 512x12 array, which Quartus builds as one true dual-port M10K. The array
// has two read ports because the phase generator reads the two tables at
// different pipeline stages. Both reads are registered with no enable and
// no reset, as in the vendor's opl2_log_sine_lut and opl2_exp_lut, so
// replacing those two modules leaves the pipeline timing unchanged.

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
