/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The Pocket's own top, from Analogue's core template: the pad ring,
 * the tie-offs for every interface this machine does not use, and the
 * bridge command block are theirs unchanged. What is ours begins where
 * their example video generator did — the clocks, the machine, and the
 * three seams it meets the host through.
 *
 * The clocks all come off one VCO so they are edge aligned: 74.25 MHz
 * in, multiplied by 448 over 33 to 1008, then divided by ten for the
 * machine, by forty for the beam, and by ten again with a phase shift
 * for the memory's own clock, which has to arrive at the chip after the
 * signals it latches.
 */

//
// User core top-level
//
// Instantiated by the real top-level: apf_top
//

`default_nettype none

module core_top #(
    /* 1 paints colour bars instead of the machine, for first bring-up. */
    parameter bit CORE_TEST_PATTERN = 1'b0,
    parameter TCM_INIT_FILE = "sw"
) (

//
// physical connections
//

///////////////////////////////////////////////////
// clock inputs 74.25mhz. not phase aligned, so treat these domains as asynchronous

input   wire            clk_74a, // mainclk1
input   wire            clk_74b, // mainclk1 

///////////////////////////////////////////////////
// cartridge interface
// switches between 3.3v and 5v mechanically
// output enable for multibit translators controlled by pic32

// GBA AD[15:8]
inout   wire    [7:0]   cart_tran_bank2,
output  wire            cart_tran_bank2_dir,

// GBA AD[7:0]
inout   wire    [7:0]   cart_tran_bank3,
output  wire            cart_tran_bank3_dir,

// GBA A[23:16]
inout   wire    [7:0]   cart_tran_bank1,
output  wire            cart_tran_bank1_dir,

// GBA [7] PHI#
// GBA [6] WR#
// GBA [5] RD#
// GBA [4] CS1#/CS#
//     [3:0] unwired
inout   wire    [7:4]   cart_tran_bank0,
output  wire            cart_tran_bank0_dir,

// GBA CS2#/RES#
inout   wire            cart_tran_pin30,
output  wire            cart_tran_pin30_dir,
// when GBC cart is inserted, this signal when low or weak will pull GBC /RES low with a special circuit
// the goal is that when unconfigured, the FPGA weak pullups won't interfere.
// thus, if GBC cart is inserted, FPGA must drive this high in order to let the level translators
// and general IO drive this pin.
output  wire            cart_pin30_pwroff_reset,

// GBA IRQ/DRQ
inout   wire            cart_tran_pin31,
output  wire            cart_tran_pin31_dir,

// infrared
input   wire            port_ir_rx,
output  wire            port_ir_tx,
output  wire            port_ir_rx_disable, 

// GBA link port
inout   wire            port_tran_si,
output  wire            port_tran_si_dir,
inout   wire            port_tran_so,
output  wire            port_tran_so_dir,
inout   wire            port_tran_sck,
output  wire            port_tran_sck_dir,
inout   wire            port_tran_sd,
output  wire            port_tran_sd_dir,
 
///////////////////////////////////////////////////
// cellular psram 0 and 1, two chips (64mbit x2 dual die per chip)

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

///////////////////////////////////////////////////
// sdram, 512mbit 16bit

output  wire    [12:0]  dram_a,
output  wire    [1:0]   dram_ba,
inout   wire    [15:0]  dram_dq,
output  wire    [1:0]   dram_dqm,
output  wire            dram_clk,
output  wire            dram_cke,
output  wire            dram_ras_n,
output  wire            dram_cas_n,
output  wire            dram_we_n,

///////////////////////////////////////////////////
// sram, 2 mbit / 256 KB, AS6C2016-55BIN

output  wire    [16:0]  sram_a,
inout   wire    [15:0]  sram_dq,
output  wire            sram_oe_n,
output  wire            sram_we_n,
output  wire            sram_ub_n,
output  wire            sram_lb_n,

///////////////////////////////////////////////////
// vblank driven by dock for sync in a certain mode

input   wire            vblank,

///////////////////////////////////////////////////
// i/o to 6515D breakout usb uart

output  wire            dbg_tx,
input   wire            dbg_rx,

///////////////////////////////////////////////////
// i/o pads near jtag connector user can solder to

output  wire            user1,
input   wire            user2,

///////////////////////////////////////////////////
// RFU internal i2c bus 

inout   wire            aux_sda,
output  wire            aux_scl,

///////////////////////////////////////////////////
// RFU, do not use
output  wire            vpll_feed,


//
// logical connections
//

///////////////////////////////////////////////////
// video, audio output to scaler
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

///////////////////////////////////////////////////
// bridge bus connection
// synchronous to clk_74a
output  wire            bridge_endian_little,
input   wire    [31:0]  bridge_addr,
input   wire            bridge_rd,
output  reg     [31:0]  bridge_rd_data,
input   wire            bridge_wr,
input   wire    [31:0]  bridge_wr_data,

///////////////////////////////////////////////////
// controller data
// 
// key bitmap:
//   [0]    dpad_up
//   [1]    dpad_down
//   [2]    dpad_left
//   [3]    dpad_right
//   [4]    face_a
//   [5]    face_b
//   [6]    face_x
//   [7]    face_y
//   [8]    trig_l1
//   [9]    trig_r1
//   [10]   trig_l2
//   [11]   trig_r2
//   [12]   trig_l3
//   [13]   trig_r3
//   [14]   face_select
//   [15]   face_start
//   [31:28] type
// joy values - unsigned
//   [ 7: 0] lstick_x
//   [15: 8] lstick_y
//   [23:16] rstick_x
//   [31:24] rstick_y
// trigger values - unsigned
//   [ 7: 0] ltrig
//   [15: 8] rtrig
//
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

// not using the IR port, so turn off both the LED, and
// disable the receive circuit to save power
assign port_ir_tx = 0;
assign port_ir_rx_disable = 1;

// bridge endianness
assign bridge_endian_little = 0;

// cart is unused, so set all level translators accordingly
// directions are 0:IN, 1:OUT
assign cart_tran_bank3 = 8'hzz;
assign cart_tran_bank3_dir = 1'b0;
assign cart_tran_bank2 = 8'hzz;
assign cart_tran_bank2_dir = 1'b0;
assign cart_tran_bank1 = 8'hzz;
assign cart_tran_bank1_dir = 1'b0;
assign cart_tran_bank0 = 4'hf;
assign cart_tran_bank0_dir = 1'b1;
assign cart_tran_pin30 = 1'b0;      // reset or cs2, we let the hw control it by itself
assign cart_tran_pin30_dir = 1'bz;
assign cart_pin30_pwroff_reset = 1'b0;  // hardware can control this
assign cart_tran_pin31 = 1'bz;      // input
assign cart_tran_pin31_dir = 1'b0;  // input

// link port is unused, set to input only to be safe
// each bit may be bidirectional in some applications
assign port_tran_so = 1'bz;
assign port_tran_so_dir = 1'b0;     // SO is output only
assign port_tran_si = 1'bz;
assign port_tran_si_dir = 1'b0;     // SI is input only
assign port_tran_sck = 1'bz;
assign port_tran_sck_dir = 1'b0;    // clock direction can change
assign port_tran_sd = 1'bz;
assign port_tran_sd_dir = 1'b0;     // SD is input and not used

// tie off the rest of the pins we are not using
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

// dram is the staging store; pocket_core drives it. Its data bus is
// split into an output, an enable and an input, because a tristate
// belongs at the pad and nowhere else.
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


// for bridge write data, we just broadcast it to all bus devices
// for bridge read data, we have to mux it
// add your own devices here
//
// 0x0xxxxxxx is the staging store, which the host only writes -- except
// the savestate blob near the top of it, which is where it reads the
// machine back out. The window at 0x2xxxxxxx is the file bridge's
// outbound buffer. Both arrive through pocket_core, which picks between
// them on the blob's exact window rather than the megabyte around it.
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


//
// host/target command handler
//
    wire            reset_n;                // driven by host commands, can be used as core-wide reset
    wire    [31:0]  cmd_bridge_rd_data;
    wire    [31:0]  file_bridge_rd_data;
    
// bridge host commands
// synchronous to clk_74a
    wire            status_boot_done = pll_locked_s;
    wire            status_setup_done = pll_locked_s; // rising edge triggers a target command
    wire            status_running = reset_n; // we are running as soon as reset_n goes high

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

    // Sleep on openFPGA is a savestate: the host asks 0x00A0 for a blob,
    // cuts the power, and hands the blob back at 0x00A4 on wake. Wake
    // reconfigures the part — measured, with a marker in memory the
    // bitstream would overwrite and a counter nothing but a reset can
    // clear, and both were gone every time — so SRAM, XRAM, TCM and
    // every register come out of the bitstream and the blob is the only
    // thing that crosses.
    //
    // pocket_sst answers the handshake and serves the blob; the firmware
    // fills it. The blob lives at the top of what the ROM slot gave up,
    // so a load arrives as ordinary bridge writes into the staging store
    // and needs no path of its own.
    //
    // The size is sst_engine's word count times four, and pocket_sst
    // has the same number: reading the last word is what tells the
    // machine the host has finished with it. stage_map_gate checks the
    // copies against each other.
    //
    // maxloadsize is not the blob's size and saying it was cost a
    // device: the OS keeps the blob wrapped in a file with its own
    // header in front and a thumbnail behind -- 378304 bytes around a
    // 324944-word... byte blob, measured -- and what comes back at a
    // load is the wrapping too. maxloadsize is the window, the engine
    // finds its magic inside whatever arrives, and an OS that sizes a
    // buffer off this number gets one its file fits in.
    //
    // The ack is answered in fabric whatever this says, because the
    // bridge's command engine parks in the savestate state until it
    // comes and a core that claims support without answering takes
    // every host command down with it.
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

// bridge target commands
// synchronous to clk_74a

// pocket_file drives these; the machine's firmware is what asks.
    wire            target_dataslot_read;
    wire            target_dataslot_write;
    wire            target_dataslot_getfile;    // require additional param/resp structs to be mapped
    wire            target_dataslot_flush;
    wire            target_dataslot_openfile;   // require additional param/resp structs to be mapped

    wire            target_dataslot_ack;
    wire            target_dataslot_done;
    wire    [2:0]   target_dataslot_err;

    wire    [15:0]  target_dataslot_id;
    wire    [31:0]  target_dataslot_slotoffset;
    wire    [31:0]  target_dataslot_bridgeaddr;
    wire    [31:0]  target_dataslot_length;

    wire    [31:0]  target_buffer_param_struct; // Open File reads the name out of the file bridge's own window
    wire    [31:0]  target_buffer_resp_struct;  // Get File answers into the staging store

// bridge data slot access
// synchronous to clk_74a

    wire    [9:0]   datatable_addr;
    // Port A is ours, both ways now: reads for slot sizes, and the
    // write that publishes a nonvolatile slot's size — the host
    // persists exactly as many bytes as the table names, so a slot
    // whose file does not exist yet stays zero until the machine says
    // otherwise. The host still writes through port B.
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



////////////////////////////////////////////////////////////////////////////////////////



// ---------------------------------------------------------------------
// the machine
// ---------------------------------------------------------------------

wire clk_sys;      //  50.4 MHz, the machine
wire clk_vid;      //  25.2 MHz, the beam
wire clk_dram;     //  50.4 MHz, half a period late, to the chip
wire clk_vid_90;   // 25.2 MHz, a quarter period on, for the scaler
wire clk_rv;       // 25.2 MHz, rising with clk_sys, the soft CPU's
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

// The chip's clock is the shifted one, and it leaves from a register in
// the pad rather than from the fabric, because that is the only way the
// shift means anything. Every other pin to the chip is launched by an
// output register; a clock carried out on an assign is launched by
// whatever routing the fitter happened to give it, so the 180 degrees
// the PLL was asked for is 180 degrees plus an unknown. Through the
// DDR output cell, high on the rise and low on the fall, it leaves the
// same way its data does and the angle survives to the pad.
pin_ddio_clk dram_clk_out (
    .datain_h ( 1'b1 ),
    .datain_l ( 1'b0 ),
    .outclock ( clk_dram ),
    .dataout  ( dram_clk )
);

// Out of reset as soon as the clocks are good, and not one moment
// later. reset_n is the host's run gate, not a reset: it goes high
// after the data slots have been streamed, and the bridge has to be
// awake through that stream to put them in the memory. Gating the
// core's reset on it means the whole image is written into a core that
// is holding its own write queue clear, and the machine wakes to an
// empty store. reset_n reaches pocket_core on its own port for the job
// it actually has.
wire core_rst_n_74 = pll_locked_s;
wire core_rst_n_sys;
synch_3 s_rst_sys (core_rst_n_74, core_rst_n_sys, clk_sys);

// The machine's clock stops at the source when the serializer asks --
// all of it, at once, wherever the machine happens to be. That is
// coherent because it is atomic: every register in the domain freezes
// on the same missing edge and resumes on the same returned one.
// Nothing inside is gated; its state is flops and SRAM, which hold
// what they have with no clock at all. The soft CPU is not here: it
// keeps its clock and is halted at its debug port instead. Still
// running: this bridge, the memory chips, the serializer, the arrays.
wire core_stop_req, core_stop_req_74;
synch_3 s_stopreq (core_stop_req, core_stop_req_74, clk_74a);
reg mach_clk_en = 1'b1;
always @(posedge clk_74a)
    mach_clk_en <= !core_stop_req_74;
// The gate itself. The ena register inside takes the enable on the
// falling edge of the clock it gates, so no period is ever shortened.
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

    // the host's loader, and the table that says how much it wrote
    .bridge_wr            ( bridge_wr ),
    .bridge_addr          ( bridge_addr ),
    .bridge_rd            ( bridge_rd ),
    .bridge_wr_data       ( bridge_wr_data ),
    .dataslot_allcomplete ( dataslot_allcomplete ),
    .dataslot_update      ( dataslot_update ),
    .reset_n              ( reset_n ),
    .pocket_core_dt_addr  ( datatable_addr ),
    .datatable_q          ( datatable_q ),

    // the host's clock, written once at boot by command 0x0090
    .rtc_epoch ( rtc_epoch_seconds ),
    .rtc_valid ( rtc_valid ),

    // the file bridge
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

    // sleep, and the Memories menu, which are the same blob
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

    // The scanout clock the scaler samples on, and the machine's own
    // console, brought out for the debug pin and for nothing else.
    .pocket_core_ready     ( ),
    .pocket_core_tx_data   ( con_tx_data ),
    .pocket_core_tx_valid  ( con_tx_valid ),
    .pocket_core_rv_tx_data  ( con_rv_data ),
    .pocket_core_rv_tx_valid ( con_rv_valid ),
    .pocket_core_rv_halted   ( )
);

/* Both consoles out the debug pin, on the host's clock so the channel
 * survives anything wrong with ours. */
wire [7:0] con_tx_data, con_rv_data;
wire con_tx_valid, con_rv_valid;

wire dbglog_event, dbglog_done;
wire [31:0] dbglog_id;

pocket_dbglog dbglog (
    .clk_mach    ( clk_mach ),
    .tx_data     ( con_tx_data ),
    .tx_valid    ( con_tx_valid ),
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
    .tx_data     ( con_tx_data ),
    .tx_valid    ( con_tx_valid ),
    .rv_tx_data  ( con_rv_data ),
    .rv_tx_valid ( con_rv_valid ),
    .clk_74a     ( clk_74a ),
    .arst_n      ( core_rst_n_74 ),
    .pocket_dbg_tx ( dbg_tx_w )
);

/* The bring-up bisect. With bars on, the machine is still built and
 * still talking out the debug pin — only the picture is replaced, so
 * one build answers both "is the video path alive" and "is the machine
 * alive" independently. */
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
