/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Savestates, which on this platform are also sleep: at sleep the host
 * asks for a blob and cuts the power, and on wake it hands the blob
 * back and the machine picks up where it was. Wake reconfigures the
 * part, so the blob is the only thing that crosses.
 *
 * This is the command half, and the firmware is in none of it. The
 * engine stops the whole machine to make a blob -- the soft CPU with
 * it, at its debug port -- so there is no firmware running to answer a
 * create with, and a design that waited for one would wait forever. The
 * bridge's command engine parks in the savestate state until the ack,
 * so that is answered here too.
 *
 * A create ends when the host reads the last word. There is no command
 * for "finished reading" and the machine cannot start again while the
 * host is still looking at its memories, so the last word is the only
 * signal there is; that is why the blob's length is a parameter here.
 *
 * A restore is the other way round: the engine does the whole thing and
 * the firmware is told afterwards, because by then the firmware is the
 * one the blob brought. Letting go of the restore is what starts the
 * 6502 again, so whatever fabric no blob carries gets put back before
 * the machine that uses it takes another cycle.
 *
 * A bridge write into the blob window raises a sticky bit, because that
 * is how a wake announces itself before any command arrives: the host
 * writes the blob in as ordinary bridge writes, which pocket_bridge
 * puts in the staging store, and only then asks for the load. A
 * firmware that finds the bit set at boot knows not to start the ROM.
 *
 * Serving the blob out is not here, and it is not a stream. The blob is
 * the machine's four memories; the thing that reads them is the
 * machine's own bus, with the machine frozen and this side holding it.
 * It was a queue fed by the firmware once, which measured at 3124 ns a
 * word against about a microsecond a word from the host -- short on
 * every word rather than in bursts, which is the one thing a queue
 * cannot absorb.
 *
 * What this side does with a read is what the bus documentation says to
 * do with one: "Upon receiving a read the core may not immediately
 * provide the read data and has up until the next read strobe to drive
 * bridge_rd_data." So a strobe asks the engine for that word and the
 * answer is driven when it arrives, which is well inside the gap at the
 * "few megabytes per second" the same page gives for the bus.
 *
 * Nothing here assumes the host reads in order. An earlier version held
 * one word ahead of a sequential reader and called a miss an underrun;
 * that was a guess about the host dressed up as a rule, and it made a
 * read of any address but the expected one into an error. Each strobe
 * is now answered on its own.
 */

module pocket_sst #(
    /* Where the host finds the blob. mmio.h carries the copy the
     * firmware reads it back through, and data.json gives up the room
     * at the top of the ROM slot; stage_map_gate.py checks all three.
     * The base is a whole megabyte, which is what lets the window
     * decode be a compare on the low twenty bits. */
    parameter logic [31:0] BLOB_BASE = 32'h03F0_0000,
    parameter logic [31:0] BLOB_WINDOW = 32'h000A_0000,
    /* sst_engine's word count, and core_top's savestate_size divided by
     * four. Reading the last of them is what ends a create. */
    parameter int BLOB_WORDS = 81236
) (
    input logic clk_sys,
    input logic stb,
    input logic we,
    input logic [27:0] addr,
    input logic [31:0] wdata,
    output logic [31:0] pocket_sst_rdata,

    input logic clk_74a,
    input logic arst_n,
    input logic bridge_wr,
    input logic bridge_rd,
    input logic [31:0] bridge_addr,
    output logic [31:0] pocket_sst_rd_data,
    output logic pocket_sst_rd_sel,

    /* The engine, on the machine's clock. It is asked for one word at a
     * time and holds it until a different one is asked for, so the
     * index goes across as a level and the answer comes back as one. */
    output logic pocket_sst_save,
    input logic sst_ready,
    output logic [17:0] pocket_sst_rd_idx,
    output logic pocket_sst_rd_t,
    input logic [31:0] sst_rdata,
    input logic sst_rvalid,

    /* A restore runs itself. This is raised by the host's command and
     * let go of by the firmware the blob brought. */
    output logic pocket_sst_load,
    input logic sst_load_done,
    input logic sst_load_err,

    /* A level the core edge-detects, held until acknowledged. */
    input logic savestate_start,
    output logic pocket_sst_start_ack,
    output logic pocket_sst_start_busy,
    output logic pocket_sst_start_ok,
    output logic pocket_sst_start_err,

    input logic savestate_load,
    output logic pocket_sst_load_ack,
    output logic pocket_sst_load_busy,
    output logic pocket_sst_load_ok,
    output logic pocket_sst_load_err
);

    localparam logic [17:0] LAST_IDX = 18'(BLOB_WORDS - 1);

    localparam logic [1:0] REG_CTL = 2'd0;

    /* --- The host's side. --- */

    /* Where in the blob the host is reading, in words. */
    logic in_window, rd_edge;
    logic [17:0] rd_idx;
    logic bridge_rd_q;
    logic [31:0] hold;

    /* The word a strobe asked for, on its way. The index goes across as
     * a level with a toggle beside it, and the answer comes back as a
     * level: the engine holds it until a different index is asked for,
     * so it is believed only after the last one has gone away. */
    logic [17:0] ask;
    logic ask_t, asked, hold_valid, seen_low, late;
    (* preserve *) logic rvalid_s1, rvalid_s2;
    (* preserve *) logic ready_s1, ready_s2;
    (* preserve *) logic ldone_s1, ldone_s2;
    (* preserve *) logic lerr_s1, lerr_s2;

    logic blob_hit;
    always_comb begin
        in_window = bridge_addr[31:20] == BLOB_BASE[31:20]
            && bridge_addr[19:0] < BLOB_WINDOW[19:0];
        blob_hit = bridge_wr && in_window;
        rd_idx = bridge_addr[19:2];
        rd_edge = bridge_rd && !bridge_rd_q && in_window;
        pocket_sst_rd_data = hold;
        pocket_sst_rd_sel = in_window;
        /* The machine is held for as long as a blob is being made, and
         * the index stands still while the engine answers it -- the
         * same held-level crossing the file bridge's parameters use. */
        pocket_sst_save = start_hold;
        pocket_sst_rd_idx = ask;
        pocket_sst_rd_t = ask_t;
    end

    logic blob_seen;
    logic start_q, load_q;
    logic start_hold, start_done;
    logic load_hold, load_kept, load_bad;
    logic load_t;

    /* Answered from the level itself. The bridge only needs a cycle of
     * it, and anything slower is a cycle the command engine spends
     * parked for no reason.
     *
     * Busy until the first word of the blob is standing here, and then
     * done for as long as it takes the host to read the rest -- the
     * documented shape is one result that stands until the next request
     * starts. */
    always_comb begin
        pocket_sst_start_ack = savestate_start;
        pocket_sst_load_ack = savestate_load;
        pocket_sst_start_busy = start_hold && !start_done;
        pocket_sst_start_ok = start_done && !late;
        pocket_sst_start_err = start_done && late;
        pocket_sst_load_busy = load_hold && !load_kept;
        pocket_sst_load_ok = load_kept && !load_bad;
        pocket_sst_load_err = load_kept && load_bad;
    end

    always_ff @(posedge clk_74a or negedge arst_n) begin
        if (!arst_n) begin
            blob_seen <= 1'b0;
            bridge_rd_q <= 1'b0;
            hold <= '0;
            ask <= '0;
            ask_t <= 1'b0;
            asked <= 1'b0;
            hold_valid <= 1'b0;
            seen_low <= 1'b0;
            late <= 1'b0;
            rvalid_s1 <= 1'b0;
            rvalid_s2 <= 1'b0;
            ready_s1 <= 1'b0;
            ready_s2 <= 1'b0;
            ldone_s1 <= 1'b0;
            ldone_s2 <= 1'b0;
            lerr_s1 <= 1'b0;
            lerr_s2 <= 1'b0;
            start_q <= 1'b0;
            load_q <= 1'b0;
            start_hold <= 1'b0;
            start_done <= 1'b0;
            load_hold <= 1'b0;
            load_kept <= 1'b0;
            load_bad <= 1'b0;
            load_t <= 1'b0;
        end else begin
            start_q <= savestate_start;
            load_q <= savestate_load;
            bridge_rd_q <= bridge_rd;
            rvalid_s1 <= sst_rvalid;
            rvalid_s2 <= rvalid_s1;
            ready_s1 <= sst_ready;
            ready_s2 <= ready_s1;
            ldone_s1 <= sst_load_done;
            ldone_s2 <= ldone_s1;
            lerr_s1 <= sst_load_err;
            lerr_s2 <= lerr_s1;

            /* The answer to the last index is still standing while the
             * engine notices a new one, so it is believed only after it
             * has gone away once. */
            if (!rvalid_s2) seen_low <= 1'b1;
            else if (seen_low && !hold_valid) begin
                hold <= sst_rdata;
                hold_valid <= 1'b1;
                /* The last word is answered, so the host has everything
                 * and the machine can have itself back. It is released
                 * here and not at the strobe that asked, because the
                 * engine is what serves the word and going idle at the
                 * strobe would mean never serving it. */
                if (asked && ask == LAST_IDX) start_hold <= 1'b0;
            end

            if (rd_edge) begin
                /* The word from the strobe before this one never
                 * arrived. The documented contract is that it had until
                 * now, so this is the core failing to keep it. */
                if (asked && !hold_valid) late <= 1'b1;
                ask <= rd_idx;
                ask_t <= !ask_t;
                asked <= 1'b1;
                hold_valid <= 1'b0;
                seen_low <= 1'b0;
            end

            /* Sticky from the first bridge write into the window until
             * the load it announced has landed, and then armed again
             * for the next one.
             *
             * It used to be sticky for the life of the core, on the
             * theory that the firmware asks it once at boot. Hardware
             * says otherwise: measured on a real wake, this reads ZERO
             * at boot every time, because the host writes the blob only
             * after Reset Exit. By the time the first word arrives the
             * ROM has been staged and is running, so the question is
             * not "was a blob here before I started" -- it never is --
             * but "has one started arriving", and that has to be
             * askable more than once. Clearing it at the end of the
             * load is what lets the firmware keep asking without the
             * answer being yes forever after the first wake. */
            if (load_hold && ldone_s2 && !load_kept) blob_seen <= 1'b0;
            if (blob_hit) blob_seen <= 1'b1;

            /* Done when there is a word waiting, not when the engine
             * has stopped the machine. The host is entitled to read the
             * moment it is told, and between the engine saying it is
             * ready and the first word standing here there are a
             * handful of clocks in which it would be read early -- and
             * a read with nothing behind it is an underrun, which is
             * the create answering an error on its own first word. */
            if (start_hold && ready_s2) start_done <= 1'b1;
            if (load_hold && ldone_s2) begin
                load_kept <= 1'b1;
                load_bad <= lerr_s2;
            end

            if (savestate_start && !start_q) begin
                start_hold <= 1'b1;
                start_done <= 1'b0;
                asked <= 1'b0;
                hold_valid <= 1'b0;
                seen_low <= 1'b0;
                late <= 1'b0;
            end
            if (savestate_load && !load_q) begin
                load_hold <= 1'b1;
                load_kept <= 1'b0;
                load_bad <= 1'b0;
                load_t <= !load_t;
            end
        end
    end

    /* --- The machine's side. --- */

    logic load_req;
    (* preserve *) logic load_s1, load_s2, load_s3;
    (* preserve *) logic seen_s1, seen_s2;
    (* preserve *) logic under_s1, under_s2;

    initial begin
        pocket_sst_rdata = '0;
        load_req = 1'b0;
        load_s1 = 1'b0;
        load_s2 = 1'b0;
        load_s3 = 1'b0;
        seen_s1 = 1'b0;
        seen_s2 = 1'b0;
        under_s1 = 1'b0;
        under_s2 = 1'b0;
    end

    always_comb pocket_sst_load = load_req;

    always_ff @(posedge clk_sys) begin
        load_s1 <= load_t;
        load_s2 <= load_s1;
        load_s3 <= load_s2;
        seen_s1 <= blob_seen;
        seen_s2 <= seen_s1;
        under_s1 <= late;
        under_s2 <= under_s1;

        if (stb && we && addr[3:2] == REG_CTL && wdata[0]) load_req <= 1'b0;

        /* After the clear, so a request that lands in the same cycle
         * the firmware acknowledges the last one is not the one that
         * gets lost. */
        if (load_s2 != load_s3) load_req <= 1'b1;

        /* The restore bit reads back as the engine's answer rather than
         * the host's question: the firmware that acts on it is the one
         * the blob brought, and it has nothing to say until the blob is
         * in. Clearing it is what lets the 6502 run again. */
        if (stb)
            pocket_sst_rdata <= addr[3:2] == REG_CTL
                ? {28'd0, sst_load_err, under_s2, seen_s2, sst_load_done}
                : 32'd0;
    end

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_pocket_sst;
    always_comb unused_pocket_sst = we ^ (^addr[27:4]) ^ (^addr[1:0])
        ^ (^wdata[31:1]) ^ (^bridge_addr[31:20]) ^ ready_s1;
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
