/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The core's top, which sys/sys_top.v instantiates as emu. It has the
 * shape of Template_MiSTer's Template.sv: every port this core does not
 * use is tied off, hps_io talks to the HPS and rtl/pll makes the clocks.
 *
 * For now the picture is a test card in the shape of the machine's pixel
 * stream, mister_bars, through mister_video, the path the machine's own
 * picture takes. The OSD picks its canvas.
 *
 * The video is each canvas at its own size and aspect, 60 Hz, progressive
 * (README.md's Video). A 320 wide canvas is 15 kHz, so an analog monitor
 * that takes only 31 kHz, forced_scandoubler in MiSTer.ini, gets the
 * scaler's picture; direct_video and 15 kHz CRTs get the core's own.
 */

module emu
(
    `include "sys/emu_ports.vh"
);

    assign ADC_BUS = 'Z;
    assign USER_OUT = '1;
    assign {UART_RTS, UART_TXD, UART_DTR} = '0;
    assign {SD_SCK, SD_MOSI, SD_CS} = 'Z;
    assign {SDRAM_DQ, SDRAM_A, SDRAM_BA, SDRAM_CLK, SDRAM_CKE, SDRAM_DQML,
            SDRAM_DQMH, SDRAM_nWE, SDRAM_nCAS, SDRAM_nRAS, SDRAM_nCS} = 'Z;
    assign {DDRAM_CLK, DDRAM_BURSTCNT, DDRAM_ADDR, DDRAM_DIN, DDRAM_BE,
            DDRAM_RD, DDRAM_WE} = '0;

    assign VGA_SL = '0;
    assign VGA_F1 = 1'b0;
    assign VGA_DISABLE = 1'b0;
    assign HDMI_FREEZE = 1'b0;
    assign HDMI_BLACKOUT = 1'b0;
    assign HDMI_BOB_DEINT = 1'b0;

    assign AUDIO_S = 1'b1;
    assign AUDIO_L = '0;
    assign AUDIO_R = '0;
    assign AUDIO_MIX = '0;

    assign LED_DISK = '0;
    assign LED_POWER = '0;
    assign BUTTONS = '0;

    /* clk_a2 and clk_ph are the machine's; the bars need only clk_sys. */
    wire clk_sys, clk_a2, clk_ph, pll_locked;
    pll pll (
        .refclk(CLK_50M),
        .rst(1'b0),
        .outclk_0(clk_sys),
        .outclk_1(clk_a2),
        .outclk_2(clk_ph),
        .locked(pll_locked)
    );

    /* Status bit 0 is the OSD's reset, and Main raises it while it loads
     * a core's boot ROMs. Test canvas lists vga.h's canvases by code. */
    `include "build_id.v"
    localparam CONF_STR = {
        "RP6502;;",
        "-;",
        "O[3:1],Test canvas,Console,320x240,320x180,640x480,640x360,",
            "320x200,640x400;",
        "R[0],Reset;",
        "V,v", `BUILD_DATE
    };

    /* new_vmode is tied low: Main is told of a new mode only by what it
     * measures, and the same canvas measures the same. */
    wire [1:0] buttons;
    wire [127:0] status;
    wire forced_scandoubler, direct_video;
    hps_io #(.CONF_STR(CONF_STR), .F12KEYMOD(1)) hps_io (
        .clk_sys(clk_sys),
        .HPS_BUS(HPS_BUS),
        .EXT_BUS(),
        .gamma_bus(),
        .forced_scandoubler(forced_scandoubler),
        .direct_video(direct_video),
        .new_vmode(1'b0),
        .buttons(buttons),
        .status(status)
    );
    assign VGA_SCALER = forced_scandoubler & ~direct_video;

    /* Nothing is reset yet; the LED shows that the reset arrives. */
    wire reset = RESET | status[0] | buttons[1];
    assign LED_USER = reset;

    wire [15:0] bars_pixel;
    wire bars_de, bars_frame;
    wire [2:0] bars_canvas;
    mister_bars bars (
        .clk_sys(clk_sys),
        .canvas(status[3:1]),
        .mister_bars_pixel(bars_pixel),
        .mister_bars_de(bars_de),
        .mister_bars_frame(bars_frame),
        .mister_bars_canvas(bars_canvas)
    );

    mister_video video (
        .clk_sys(clk_sys),
        .vid_pixel(bars_pixel),
        .vid_de(bars_de),
        .vid_frame(bars_frame),
        .vid_canvas(bars_canvas),
        .mister_video_ce(CE_PIXEL),
        .mister_video_r(VGA_R),
        .mister_video_g(VGA_G),
        .mister_video_b(VGA_B),
        .mister_video_hs(VGA_HS),
        .mister_video_vs(VGA_VS),
        .mister_video_de(VGA_DE),
        .mister_video_arx(VIDEO_ARX),
        .mister_video_ary(VIDEO_ARY)
    );
    assign CLK_VIDEO = clk_sys;

endmodule
