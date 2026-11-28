/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * mister_bars into mister_video, as RP6502.sv joins them for M1.
 * frame_en low hides vid_frame from mister_video, a machine that never
 * starts a frame.
 */

module tb_mvideo (
    input logic clk_sys,
    input logic [2:0] canvas,
    input logic frame_en,

    output logic tb_mvideo_ce,
    output logic [7:0] tb_mvideo_r,
    output logic [7:0] tb_mvideo_g,
    output logic [7:0] tb_mvideo_b,
    output logic tb_mvideo_hs,
    output logic tb_mvideo_vs,
    output logic tb_mvideo_de,
    output logic [12:0] tb_mvideo_arx,
    output logic [12:0] tb_mvideo_ary
);

    logic [15:0] pixel;
    logic de, frame;
    logic [2:0] cv;
    mister_bars bars (
        .clk_sys(clk_sys),
        .canvas(canvas),
        .mister_bars_pixel(pixel),
        .mister_bars_de(de),
        .mister_bars_frame(frame),
        .mister_bars_canvas(cv)
    );

    mister_video video (
        .clk_sys(clk_sys),
        .vid_pixel(pixel),
        .vid_de(de),
        .vid_frame(frame && frame_en),
        .vid_canvas(cv),
        .mister_video_ce(tb_mvideo_ce),
        .mister_video_r(tb_mvideo_r),
        .mister_video_g(tb_mvideo_g),
        .mister_video_b(tb_mvideo_b),
        .mister_video_hs(tb_mvideo_hs),
        .mister_video_vs(tb_mvideo_vs),
        .mister_video_de(tb_mvideo_de),
        .mister_video_arx(tb_mvideo_arx),
        .mister_video_ary(tb_mvideo_ary)
    );

endmodule
