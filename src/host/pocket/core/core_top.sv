/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

`default_nettype none

module core_top #(
    parameter bit CORE_TEST_PATTERN = 1'b0,
    parameter TCM_INIT_FILE = "sw"
) (

input   wire            clk_74a,
input   wire            clk_74b,

inout   wire    [7:0]   cart_tran_bank2,
output  wire            cart_tran_bank2_dir,

inout   wire    [7:0]   cart_tran_bank3,
output  wire            cart_tran_bank3_dir,

inout   wire    [7:0]   cart_tran_bank1,
output  wire            cart_tran_bank1_dir,

inout   wire    [7:4]   cart_tran_bank0,
output  wire            cart_tran_bank0_dir,

inout   wire            cart_tran_pin30,
output  wire            cart_tran_pin30_dir,
output  wire            cart_pin30_pwroff_reset,

inout   wire            cart_tran_pin31,
output  wire            cart_tran_pin31_dir,

input   wire            port_ir_rx,
output  wire            port_ir_tx,
output  wire            port_ir_rx_disable, 

inout   wire            port_tran_si,
output  wire            port_tran_si_dir,
inout   wire            port_tran_so,
output  wire            port_tran_so_dir,
inout   wire            port_tran_sck,
output  wire            port_tran_sck_dir,
inout   wire            port_tran_sd,
output  wire            port_tran_sd_dir,
 
output  wire    [21:16] cram0_a,
inout   wire    [15:0]  cram0_dq,
input   wire            cram0_wait,
output  wire            cram0_clk,
output  wire            cram0_adv_n,
output  wire            cram0_cre,
output  wire            cram0_ce0_n,
output  wire            cram0_ce1_n,
output  wire            cram0_oe_n,
output  wire            cram0_we_n,
output  wire            cram0_ub_n,
output  wire            cram0_lb_n,

output  wire    [21:16] cram1_a,
inout   wire    [15:0]  cram1_dq,
input   wire            cram1_wait,
output  wire            cram1_clk,
output  wire            cram1_adv_n,
output  wire            cram1_cre,
output  wire            cram1_ce0_n,
output  wire            cram1_ce1_n,
output  wire            cram1_oe_n,
output  wire            cram1_we_n,
output  wire            cram1_ub_n,
output  wire            cram1_lb_n,

output  wire    [12:0]  dram_a,
output  wire    [1:0]   dram_ba,
inout   wire    [15:0]  dram_dq,
output  wire    [1:0]   dram_dqm,
output  wire            dram_clk,
output  wire            dram_cke,
output  wire            dram_ras_n,
output  wire            dram_cas_n,
output  wire            dram_we_n,

output  wire    [16:0]  sram_a,
inout   wire    [15:0]  sram_dq,
output  wire            sram_oe_n,
output  wire            sram_we_n,
output  wire            sram_ub_n,
output  wire            sram_lb_n,

input   wire            vblank,

output  wire            dbg_tx,
input   wire            dbg_rx,

output  wire            user1,
input   wire            user2,

inout   wire            aux_sda,
output  wire            aux_scl,

output  wire            vpll_feed,

output  wire    [23:0]  video_rgb,
output  wire            video_rgb_clock,
output  wire            video_rgb_clock_90,
output  wire            video_de,
output  wire            video_skip,
output  wire            video_vs,
output  wire            video_hs,
    
output  wire            audio_mclk,
input   wire            audio_adc,
output  wire            audio_dac,
output  wire            audio_lrck,

output  wire            bridge_endian_little,
input   wire    [31:0]  bridge_addr,
input   wire            bridge_rd,
output  reg     [31:0]  bridge_rd_data,
input   wire            bridge_wr,
input   wire    [31:0]  bridge_wr_data,

input   wire    [31:0]  cont1_key,
input   wire    [31:0]  cont2_key,
input   wire    [31:0]  cont3_key,
input   wire    [31:0]  cont4_key,
input   wire    [31:0]  cont1_joy,
input   wire    [31:0]  cont2_joy,
input   wire    [31:0]  cont3_joy,
input   wire    [31:0]  cont4_joy,
input   wire    [15:0]  cont1_trig,
input   wire    [15:0]  cont2_trig,
input   wire    [15:0]  cont3_trig,
input   wire    [15:0]  cont4_trig
    
);

assign port_ir_tx = 0;
assign port_ir_rx_disable = 1;

assign bridge_endian_little = 0;

assign cart_tran_bank3 = 8'hzz;
assign cart_tran_bank3_dir = 1'b0;
assign cart_tran_bank2 = 8'hzz;
assign cart_tran_bank2_dir = 1'b0;
assign cart_tran_bank1 = 8'hzz;
assign cart_tran_bank1_dir = 1'b0;
assign cart_tran_bank0 = 4'hf;
assign cart_tran_bank0_dir = 1'b1;
assign cart_tran_pin30 = 1'b0;
assign cart_tran_pin30_dir = 1'bz;
assign cart_pin30_pwroff_reset = 1'b0;
assign cart_tran_pin31 = 1'bz;
assign cart_tran_pin31_dir = 1'b0;

assign port_tran_so = 1'bz;
assign port_tran_so_dir = 1'b0;
assign port_tran_si = 1'bz;
assign port_tran_si_dir = 1'b0;
assign port_tran_sck = 1'bz;
assign port_tran_sck_dir = 1'b0;
assign port_tran_sd = 1'bz;
assign port_tran_sd_dir = 1'b0;

assign cram0_a = 'h0;
assign cram0_dq = {16{1'bZ}};
assign cram0_clk = 0;
assign cram0_adv_n = 1;
assign cram0_cre = 0;
assign cram0_ce0_n = 1;
assign cram0_ce1_n = 1;
assign cram0_oe_n = 1;
assign cram0_we_n = 1;
assign cram0_ub_n = 1;
assign cram0_lb_n = 1;

assign cram1_a = 'h0;
assign cram1_dq = {16{1'bZ}};
assign cram1_clk = 0;
assign cram1_adv_n = 1;
assign cram1_cre = 0;
assign cram1_ce0_n = 1;
assign cram1_ce1_n = 1;
assign cram1_oe_n = 1;
assign cram1_we_n = 1;
assign cram1_ub_n = 1;
assign cram1_lb_n = 1;

wire [15:0] dram_dq_out;
wire        dram_dq_oe;
assign dram_dq = dram_dq_oe ? dram_dq_out : {16{1'bZ}};

wire [15:0] sram_dq_out;
wire        sram_dq_oe;
assign sram_dq = sram_dq_oe ? sram_dq_out : {16{1'bZ}};

wire dbg_tx_w;
assign dbg_tx = dbg_tx_w;
assign user1 = 1'bZ;
assign aux_scl = 1'bZ;
assign vpll_feed = 1'bZ;


// 0x03Fxxxxx is the megabyte that starts at savestate_addr. Its first
// savestate_maxloadsize bytes are the blob window, and for reads inside
// that window pocket_core returns pocket_sst's data on
// file_bridge_rd_data.
always @(*) begin
    casex(bridge_addr)
    default: begin
        bridge_rd_data <= 0;
    end
    32'h03Fxxxxx: begin
        bridge_rd_data <= file_bridge_rd_data;
    end
    32'h20xxxxxx: begin
        bridge_rd_data <= file_bridge_rd_data;
    end
    32'hF8xxxxxx: begin
        bridge_rd_data <= cmd_bridge_rd_data;
    end
    endcase
end

    wire            reset_n;
    wire    [31:0]  cmd_bridge_rd_data;
    wire    [31:0]  file_bridge_rd_data;
    
    wire            status_boot_done = pll_locked_s;
    wire            status_setup_done = pll_locked_s;
    wire            status_running = reset_n;

    wire            dataslot_requestread;
    wire    [15:0]  dataslot_requestread_id;
    wire            dataslot_requestread_ack = 1;
    wire            dataslot_requestread_ok = 1;

    wire            dataslot_requestwrite;
    wire    [15:0]  dataslot_requestwrite_id;
    wire    [31:0]  dataslot_requestwrite_size;
    wire            dataslot_requestwrite_ack = 1;
    wire            dataslot_requestwrite_ok = 1;

    wire            dataslot_update;
    
    wire            dataslot_allcomplete;

    wire     [31:0] rtc_epoch_seconds;
    wire     [31:0] rtc_date_bcd;
    wire     [31:0] rtc_time_bcd;
    wire            rtc_valid;

    // savestate_size is sst_engine's word count times four. pocket_sst
    // has the same word count because it drops its save request when the
    // last word of the blob arrives from sst_engine, and sst_engine
    // resumes the machine once that request drops. savestate_maxloadsize
    // is the whole blob window rather than the blob's size, because the
    // host writes back the file it saved, which has the Pocket OS's header
    // in front of the blob and a thumbnail behind it, and sst_engine scans
    // that file for the blob's magic. One saved file measured 378304
    // bytes. stage_map_gate.py checks the size against sst_engine and
    // pocket_sst, and the address and the load size against mmio.h.
    wire            savestate_supported = 1'b1;
    wire    [31:0]  savestate_addr = 32'h03F0_0000;
    wire    [31:0]  savestate_size = 32'd324944;
    wire    [31:0]  savestate_maxloadsize = 32'd655360;

    wire            savestate_start;
    wire            savestate_start_ack;
    wire            savestate_start_busy;
    wire            savestate_start_ok;
    wire            savestate_start_err;

    wire            savestate_load;
    wire            savestate_load_ack;
    wire            savestate_load_busy;
    wire            savestate_load_ok;
    wire            savestate_load_err;

    wire            osnotify_inmenu;

    wire            target_dataslot_read;
    wire            target_dataslot_write;
    wire            target_dataslot_getfile;
    wire            target_dataslot_flush;
    wire            target_dataslot_openfile;

    wire            target_dataslot_ack;
    wire            target_dataslot_done;
    wire    [2:0]   target_dataslot_err;

    wire    [15:0]  target_dataslot_id;
    wire    [31:0]  target_dataslot_slotoffset;
    wire    [31:0]  target_dataslot_bridgeaddr;
    wire    [31:0]  target_dataslot_length;

    wire    [31:0]  target_buffer_param_struct;
    wire    [31:0]  target_buffer_resp_struct;

    wire    [9:0]   datatable_addr;
    wire            datatable_wren;
    wire    [31:0]  datatable_data;
    wire    [31:0]  datatable_q;

core_bridge_cmd icb (

    .clk                ( clk_74a ),
    .reset_n            ( reset_n ),

    .bridge_endian_little   ( bridge_endian_little ),
    .bridge_addr            ( bridge_addr ),
    .bridge_rd              ( bridge_rd ),
    .bridge_rd_data         ( cmd_bridge_rd_data ),
    .bridge_wr              ( bridge_wr ),
    .bridge_wr_data         ( bridge_wr_data ),
    
    .status_boot_done       ( status_boot_done ),
    .status_setup_done      ( status_setup_done ),
    .status_running         ( status_running ),

    .dataslot_requestread       ( dataslot_requestread ),
    .dataslot_requestread_id    ( dataslot_requestread_id ),
    .dataslot_requestread_ack   ( dataslot_requestread_ack ),
    .dataslot_requestread_ok    ( dataslot_requestread_ok ),

    .dataslot_requestwrite      ( dataslot_requestwrite ),
    .dataslot_requestwrite_id   ( dataslot_requestwrite_id ),
    .dataslot_requestwrite_size ( dataslot_requestwrite_size ),
    .dataslot_requestwrite_ack  ( dataslot_requestwrite_ack ),
    .dataslot_requestwrite_ok   ( dataslot_requestwrite_ok ),

    .dataslot_update            ( dataslot_update ),
    
    .dataslot_allcomplete   ( dataslot_allcomplete ),

    .rtc_epoch_seconds      ( rtc_epoch_seconds ),
    .rtc_date_bcd           ( rtc_date_bcd ),
    .rtc_time_bcd           ( rtc_time_bcd ),
    .rtc_valid              ( rtc_valid ),
    
    .savestate_supported    ( savestate_supported ),
    .savestate_addr         ( savestate_addr ),
    .savestate_size         ( savestate_size ),
    .savestate_maxloadsize  ( savestate_maxloadsize ),

    .savestate_start        ( savestate_start ),
    .savestate_start_ack    ( savestate_start_ack ),
    .savestate_start_busy   ( savestate_start_busy ),
    .savestate_start_ok     ( savestate_start_ok ),
    .savestate_start_err    ( savestate_start_err ),

    .savestate_load         ( savestate_load ),
    .savestate_load_ack     ( savestate_load_ack ),
    .savestate_load_busy    ( savestate_load_busy ),
    .savestate_load_ok      ( savestate_load_ok ),
    .savestate_load_err     ( savestate_load_err ),

    .osnotify_inmenu        ( osnotify_inmenu ),
    
    .target_debug_event         ( dbglog_event ),
    .target_debug_id            ( dbglog_id ),
    .target_debug_done          ( dbglog_done ),

    .target_dataslot_read       ( target_dataslot_read ),
    .target_dataslot_write      ( target_dataslot_write ),
    .target_dataslot_getfile    ( target_dataslot_getfile ),
    .target_dataslot_flush      ( target_dataslot_flush ),
    .target_dataslot_openfile   ( target_dataslot_openfile ),
    
    .target_dataslot_ack        ( target_dataslot_ack ),
    .target_dataslot_done       ( target_dataslot_done ),
    .target_dataslot_err        ( target_dataslot_err ),

    .target_dataslot_id         ( target_dataslot_id ),
    .target_dataslot_slotoffset ( target_dataslot_slotoffset ),
    .target_dataslot_bridgeaddr ( target_dataslot_bridgeaddr ),
    .target_dataslot_length     ( target_dataslot_length ),

    .target_buffer_param_struct ( target_buffer_param_struct ),
    .target_buffer_resp_struct  ( target_buffer_resp_struct ),
    
    .datatable_addr         ( datatable_addr ),
    .datatable_wren         ( datatable_wren ),
    .datatable_data         ( datatable_data ),
    .datatable_q            ( datatable_q )

);

wire clk_sys;      // 50.4 MHz
wire clk_vid;      // 25.2 MHz
wire clk_dram;     // 50.4 MHz, 180 degrees behind clk_sys
wire clk_vid_90;   // 25.2 MHz, 90 degrees behind clk_vid
wire clk_rv;       // 25.2 MHz, rising with clk_sys
wire pll_locked;
wire pll_locked_s;
synch_3 s_pll (pll_locked, pll_locked_s, clk_74a);

pocket_pll pll (
    .refclk   ( clk_74a ),
    .rst      ( 1'b0 ),
    .clk_sys  ( clk_sys ),
    .clk_vid  ( clk_vid ),
    .clk_dram ( clk_dram ),
    .clk_vid_90 ( clk_vid_90 ),
    .clk_rv   ( clk_rv ),
    .locked   ( pll_locked )
);

// dram_clk leaves from a DDR output register in the I/O cell, which
// drives it high on the rising edge of clk_dram and low on the falling
// edge. Every other SDRAM pin except dram_dqm, which is tied low, also
// leaves from an I/O cell register, so the clock and the signals the
// SDRAM samples on it take the same kind of path to the pads, and the
// 180-degree shift set in pocket_pll is preserved at the chip. A clock
// driven onto the pin by an assign would take a fabric route chosen by
// the fitter, which adds an unknown delay to that shift.
pin_ddio_clk dram_clk_out (
    .datain_h ( 1'b1 ),
    .datain_l ( 1'b0 ),
    .outclock ( clk_dram ),
    .dataout  ( dram_clk )
);

// reset_n rises only when the host sends Reset Exit (0x0011), after it
// has written the data slots, and pocket_bridge has to be out of reset
// during those writes to queue them for the SDRAM. pocket_core's resets
// therefore follow the PLL lock, and reset_n goes to pocket_core as a
// separate input that holds the machine in reset while it is low.
wire core_rst_n_74 = pll_locked_s;
wire core_rst_n_sys;
synch_3 s_rst_sys (core_rst_n_74, core_rst_n_sys, clk_sys);

// The clock control block below stops clk_mach for a savestate, so every
// register on clk_mach misses the same edge and resumes on the same edge,
// and no register needs a clock enable for the stop. The soft CPU runs on
// clk_rv, which is not gated, and sst_engine halts it through the soft
// CPU's debug port instead.
wire core_stop_req, core_stop_req_74;
synch_3 s_stopreq (core_stop_req, core_stop_req_74, clk_74a);
reg mach_clk_en = 1'b1;
always @(posedge clk_74a)
    mach_clk_en <= !core_stop_req_74;
// altclkctrl registers ena on the falling edge of clk_sys, so clk_mach
// stops or restarts only while clk_sys is low and no high phase of
// clk_mach is ever cut short.
wire clk_mach;
altclkctrl #(
    .clock_type("AUTO"),
    .ena_register_mode("falling edge"),
    .number_of_clocks(1),
    .use_glitch_free_switch_over_implementation("OFF"),
    .width_clkselect(1)
) gate_sys ( .inclk(clk_sys), .ena(mach_clk_en), .clkselect(1'b0),
             .outclk(clk_mach) );

pocket_core #(.TCM_INIT_FILE(TCM_INIT_FILE)) core (
    .mach_running ( mach_clk_en ),
    .clk_mach     ( clk_mach ),
    .pocket_core_stop_req    ( core_stop_req ),
    .clk_74a  ( clk_74a ),
    .clk_sys  ( clk_sys ),
    .clk_rv   ( clk_rv ),
    .clk_vid  ( clk_vid ),
    .rst_n    ( core_rst_n_sys ),
    .arst_n   ( core_rst_n_74 ),

    .bridge_wr            ( bridge_wr ),
    .bridge_addr          ( bridge_addr ),
    .bridge_rd            ( bridge_rd ),
    .bridge_wr_data       ( bridge_wr_data ),
    .dataslot_allcomplete ( dataslot_allcomplete ),
    .dataslot_update      ( dataslot_update ),
    .reset_n              ( reset_n ),
    .pocket_core_dt_addr  ( datatable_addr ),
    .datatable_q          ( datatable_q ),

    .rtc_epoch ( rtc_epoch_seconds ),
    .rtc_valid ( rtc_valid ),

    .pocket_core_bridge_rd_data      ( file_bridge_rd_data ),
    .pocket_core_param_struct        ( target_buffer_param_struct ),
    .pocket_core_resp_struct         ( target_buffer_resp_struct ),
    .pocket_core_dataslot_read       ( target_dataslot_read ),
    .pocket_core_dataslot_write      ( target_dataslot_write ),
    .pocket_core_dataslot_openfile   ( target_dataslot_openfile ),
    .pocket_core_dataslot_getfile    ( target_dataslot_getfile ),
    .pocket_core_dataslot_flush      ( target_dataslot_flush ),
    .pocket_core_dataslot_id         ( target_dataslot_id ),
    .pocket_core_dataslot_slotoffset ( target_dataslot_slotoffset ),
    .pocket_core_dataslot_bridgeaddr ( target_dataslot_bridgeaddr ),
    .pocket_core_dataslot_length     ( target_dataslot_length ),
    .target_dataslot_done            ( target_dataslot_done ),
    .target_dataslot_err             ( target_dataslot_err ),

    .savestate_start                    ( savestate_start ),
    .pocket_core_savestate_start_ack    ( savestate_start_ack ),
    .pocket_core_savestate_start_busy   ( savestate_start_busy ),
    .pocket_core_savestate_start_ok     ( savestate_start_ok ),
    .pocket_core_savestate_start_err    ( savestate_start_err ),
    .savestate_load                     ( savestate_load ),
    .pocket_core_savestate_load_ack     ( savestate_load_ack ),
    .pocket_core_savestate_load_busy    ( savestate_load_busy ),
    .pocket_core_savestate_load_ok      ( savestate_load_ok ),
    .pocket_core_savestate_load_err     ( savestate_load_err ),

    .cont_key  ( {cont4_key,  cont3_key,  cont2_key,  cont1_key}  ),
    .cont_joy  ( {cont4_joy,  cont3_joy,  cont2_joy,  cont1_joy}  ),
    .cont_trig ( {cont4_trig, cont3_trig, cont2_trig, cont1_trig} ),

    .pocket_core_rgb  ( m_rgb ),
    .pocket_core_de   ( m_de ),
    .pocket_core_skip ( m_skip ),
    .pocket_core_vs   ( m_vs ),
    .pocket_core_hs   ( m_hs ),

    .pocket_core_mclk ( audio_mclk ),
    .pocket_core_dac  ( audio_dac ),
    .pocket_core_lrck ( audio_lrck ),

    .dram_cke    ( dram_cke ),
    .dram_a      ( dram_a ),
    .dram_ba     ( dram_ba ),
    .dram_dqm    ( dram_dqm ),
    .dram_ras_n  ( dram_ras_n ),
    .dram_cas_n  ( dram_cas_n ),
    .dram_we_n   ( dram_we_n ),
    .dram_dq_out ( dram_dq_out ),
    .dram_dq_oe  ( dram_dq_oe ),

    .sram_a      ( sram_a ),
    .sram_dq_out ( sram_dq_out ),
    .sram_dq_oe  ( sram_dq_oe ),
    .sram_dq_in  ( sram_dq ),
    .sram_oe_n   ( sram_oe_n ),
    .sram_we_n   ( sram_we_n ),
    .sram_ub_n   ( sram_ub_n ),
    .sram_lb_n   ( sram_lb_n ),
    .dram_dq_in  ( dram_dq ),

    .pocket_core_ready     ( ),
    .pocket_core_tx_data   ( ),
    .pocket_core_tx_valid  ( ),
    .pocket_core_rv_tx_data  ( con_rv_data ),
    .pocket_core_rv_tx_valid ( con_rv_valid ),
    .pocket_core_rv_halted   ( )
);

wire [7:0] con_rv_data;
wire con_rv_valid;

wire dbglog_event, dbglog_done;
wire [31:0] dbglog_id;

pocket_dbglog dbglog (
    .clk_mach    ( clk_mach ),
    .rv_tx_data  ( con_rv_data ),
    .rv_tx_valid ( con_rv_valid ),
    .clk_74a     ( clk_74a ),
    .arst_n      ( core_rst_n_74 ),
    .bridge_wr            ( bridge_wr ),
    .bridge_endian_little ( bridge_endian_little ),
    .bridge_addr          ( bridge_addr ),
    .bridge_wr_data       ( bridge_wr_data ),
    .target_debug_done   ( dbglog_done ),
    .pocket_dbglog_event ( dbglog_event ),
    .pocket_dbglog_id    ( dbglog_id )
);

pocket_dbg dbg (
    .clk_mach    ( clk_mach ),
    .rst_n       ( core_rst_n_sys ),
    .rv_tx_data  ( con_rv_data ),
    .rv_tx_valid ( con_rv_valid ),
    .clk_74a     ( clk_74a ),
    .arst_n      ( core_rst_n_74 ),
    .pocket_dbg_tx ( dbg_tx_w )
);

wire [23:0] m_rgb;
wire m_de, m_skip, m_vs, m_hs;
wire [23:0] b_rgb;
wire b_de, b_skip, b_vs, b_hs;

pocket_bars bars (
    .clk_vid ( clk_vid ),
    .pocket_bars_rgb  ( b_rgb ),
    .pocket_bars_de   ( b_de ),
    .pocket_bars_skip ( b_skip ),
    .pocket_bars_vs   ( b_vs ),
    .pocket_bars_hs   ( b_hs )
);

assign video_rgb  = CORE_TEST_PATTERN ? b_rgb  : m_rgb;
assign video_de   = CORE_TEST_PATTERN ? b_de   : m_de;
assign video_skip = CORE_TEST_PATTERN ? b_skip : m_skip;
assign video_vs   = CORE_TEST_PATTERN ? b_vs   : m_vs;
assign video_hs   = CORE_TEST_PATTERN ? b_hs   : m_hs;

assign video_rgb_clock = clk_vid;
assign video_rgb_clock_90 = clk_vid_90;

endmodule
