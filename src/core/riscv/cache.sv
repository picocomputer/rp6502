/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The soft CPU's memory is lines of four words that live behind the line
 * port. This cache keeps SETS * WAYS of them in block memory and answers
 * a hit in the data phase with no wait.
 *
 * Memories. Four byte lanes hold the data, one byte for every way at
 * address {set, word of the line}. The tags hold {valid, tag} for every
 * way of a set, and each set's row its ways' dirty bits and ages: an age
 * is 0 for the way used last and WAYS-1 for the way used least recently,
 * which is the one a miss replaces. A row is written whole, so it needs no
 * write enable for each way: a tag with the dirty bit in it is wider than
 * the ten bits of a block memory's byte enable. They are read at one
 * address and written at another on the same edge. A read of bytes that
 * the same edge writes is not guaranteed to return either value, so the
 * written bytes are kept in lane_fwd_data, tag_fwd_data and row_fwd_data
 * and take the place of the read ones a clock later. lane_v, tag_v and
 * row_v are the memories as the logic sees them. The Verilator model returns the complement of the
 * written bytes from such a read, so a test fails if anything uses them.
 *
 * Least recently used, rather than a pointer that goes round, because
 * which ways a set keeps then follows from its last few uses alone. A
 * pointer remembers every miss the set ever had, so a burst of other work,
 * a savestate's, could leave a loop that fits the cache missing on every
 * pass for good where the same loop before it had no miss.
 *
 * S_RUN. The memories are read at req_word in every clock, which is the
 * address phase of the next access. With dph high the tags pick the way:
 * a load gets its word, a store writes its strobed bytes and sets the
 * way's dirty bit, the way becomes the set's youngest, and cache_ready is
 * high. If no way matches,
 * cache_ready is low, nothing is written and the miss starts. Whatever
 * else leaves S_RUN leaves through S_REPLAY, so the rule that the
 * memories were read at dph_word a clock before holds for any access
 * that had to wait.
 *
 * A miss, with the CPU held:
 *   S_TAG    reads the set's tags and row again, and word 0 of the set.
 *            The copy from the address phase belongs to the next access
 *            by now.
 *   S_VIC    the victim is the oldest way. A valid and dirty victim
 *            goes to S_RD with its word 0 taken, any other to S_FILL.
 *   S_RD     three clocks take words 1 to 3 into line_wdata.
 *   S_WO     writes the victim's line through the line port.
 *   S_FILL   reads the wanted line through the line port.
 *   S_CP     four clocks copy line_rdata into the victim's way. The last
 *            writes the tag, valid, and clears the way's dirty bit.
 *   S_REPLAY reads the memories at dph_word.
 *   S_RUN    the access hits, which makes the filled way the youngest,
 *            and the next address phase is this clock.
 *
 * Reset walks S_EMPTY, one set a clock, which writes every tag invalid,
 * every way clean, and the ages so that way 0 is the oldest and fills
 * first. An access that arrives during the walk waits for
 * S_REPLAY. */
/* One memory holding an entry for every way, with a write enable for
 * each. Quartus packs the ways into one block memory with byte enables only
 * when the writes are spelled out a way at a time under one write enable;
 * written as a loop it builds the memory from registers.
 *
 * Nothing may use an entry read on the edge that writes it. In simulation
 * such an entry comes back complemented, so a test fails if anything does. */
/* verilator lint_off DECLFILENAME */
module cache_mem #(
    parameter int WAYS = 4,
    parameter int AW = 9,
    parameter int W = 8
) (
    input logic clk,
    input logic [AW-1:0] raddr,
    input logic [AW-1:0] waddr,
    input logic [WAYS-1:0] we,
    input logic [W-1:0] wdata,
    output logic [WAYS-1:0][W-1:0] cache_mem_q
);

    (* ramstyle = "no_rw_check" *)
    logic [WAYS-1:0][W-1:0] mem[1 << AW];
    logic [WAYS-1:0][W-1:0] q;

    generate
    if (WAYS == 8) begin : ways8
        always_ff @(posedge clk) begin
            if (|we) begin
                if (we[0]) mem[waddr][0] <= wdata;
                if (we[1]) mem[waddr][1] <= wdata;
                if (we[2]) mem[waddr][2] <= wdata;
                if (we[3]) mem[waddr][3] <= wdata;
                if (we[4]) mem[waddr][4] <= wdata;
                if (we[5]) mem[waddr][5] <= wdata;
                if (we[6]) mem[waddr][6] <= wdata;
                if (we[7]) mem[waddr][7] <= wdata;
            end
            q <= mem[raddr];
        end
    end else if (WAYS == 4) begin : ways4
        always_ff @(posedge clk) begin
            if (|we) begin
                if (we[0]) mem[waddr][0] <= wdata;
                if (we[1]) mem[waddr][1] <= wdata;
                if (we[2]) mem[waddr][2] <= wdata;
                if (we[3]) mem[waddr][3] <= wdata;
            end
            q <= mem[raddr];
        end
    end else begin : ways2
        always_ff @(posedge clk) begin
            if (|we) begin
                if (we[0]) mem[waddr][0] <= wdata;
                if (we[1]) mem[waddr][1] <= wdata;
            end
            q <= mem[raddr];
        end
    end
    endgenerate

`ifdef VERILATOR
    logic [WAYS-1:0] rw;
    logic [W-1:0] wq;
    always_ff @(posedge clk) begin
        rw <= waddr == raddr ? we : '0;
        wq <= wdata;
    end
    always_comb
        for (int w = 0; w < WAYS; w++)
            cache_mem_q[w] = rw[w] ? ~wq : q[w];
`else
    always_comb cache_mem_q = q;
`endif

endmodule
/* verilator lint_on DECLFILENAME */

module cache #(
    parameter int SETS = 64,
    parameter int WAYS = 8,
    parameter int MEM_WORDS = 131072
) (
    input logic clk,
    input logic rst_n,

    input logic [$clog2(MEM_WORDS)-1:0] req_word,
    input logic dph,
    input logic dph_write,
    input logic [$clog2(MEM_WORDS)-1:0] dph_word,
    input logic [3:0] dph_strb,
    input logic [31:0] wdata,
    output logic [31:0] cache_rdata,
    output logic cache_ready,

    output logic cache_line_pend,
    output logic cache_line_we,
    output logic [$clog2(MEM_WORDS)-3:0] cache_line_addr,
    output logic [127:0] cache_line_wdata,
    input logic line_done,
    input logic [127:0] line_rdata
);

    localparam int WORD_AW = $clog2(MEM_WORDS);
    localparam int SET_AW = $clog2(SETS);
    localparam int WAY_AW = $clog2(WAYS);
    localparam int TAG_W = WORD_AW - 2 - SET_AW;

    typedef struct packed {
        logic valid;
        logic [TAG_W-1:0] tag;
    } tag_t;

    typedef struct packed {
        logic [WAYS-1:0] dirty;
        logic [WAYS-1:0][WAY_AW-1:0] age;
    } row_t;

    typedef enum logic [3:0] {
        S_EMPTY,
        S_REPLAY,
        S_RUN,
        S_TAG,
        S_VIC,
        S_RD,
        S_WO,
        S_FILL,
        S_CP
    } state_t;

    state_t state;
    /* In S_EMPTY the set being worked on is walk_set and not the set of
     * the CPU's access. */
    logic [SET_AW-1:0] walk_set;
    logic [WAY_AW-1:0] way;
    logic [TAG_W-1:0] vic_tag;
    logic [1:0] n;

    logic [TAG_W-1:0] dph_tag;
    logic [SET_AW-1:0] dph_set;
    logic [1:0] dph_off;
    always_comb {dph_tag, dph_set, dph_off} = dph_word;

    logic [SET_AW-1:0] set;
    always_comb set = state == S_EMPTY ? walk_set : dph_set;

    /* The tag of the next access is not looked at until its data phase. */
    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_bits;
    always_comb unused_bits = ^req_word[WORD_AW-1:SET_AW+2];
    /* verilator lint_on UNUSEDSIGNAL */

    logic [SET_AW-1:0] rd_set;
    logic [1:0] rd_off;
    always_comb begin
        case (state)
            S_RUN: {rd_set, rd_off} = req_word[SET_AW+1:0];
            S_REPLAY: {rd_set, rd_off} = {dph_set, dph_off};
            S_TAG: {rd_set, rd_off} = {set, 2'd0};
            default: {rd_set, rd_off} = {set, n};
        endcase
    end

    logic [3:0][WAYS-1:0] lane_we;
    logic [3:0][7:0] lane_wdata;
    logic [SET_AW+1:0] lane_waddr;
    logic [3:0][WAYS-1:0][7:0] lane_q;
    logic [3:0][WAYS-1:0] lane_fwd;
    logic [3:0][7:0] lane_fwd_data;
    logic [WAYS-1:0][31:0] lane_v;

    logic [WAYS-1:0] tag_we;
    tag_t tag_wdata;
    logic [WAYS-1:0][TAG_W:0] tag_q;
    logic [WAYS-1:0] tag_fwd;
    tag_t tag_fwd_data;
    tag_t [WAYS-1:0] tag_v;

    logic row_we;
    row_t row_wdata;
    row_t row_q;
    logic row_fwd;
    row_t row_fwd_data;
    row_t row_v;

    genvar g;
    generate
    for (g = 0; g < 4; g++) begin : lane
        cache_mem #(.WAYS(WAYS), .AW(SET_AW + 2), .W(8)) lane_mem (
            .clk(clk),
            .raddr({rd_set, rd_off}),
            .waddr(lane_waddr),
            .we(lane_we[g]),
            .wdata(lane_wdata[g]),
            .cache_mem_q(lane_q[g])
        );
    end
    endgenerate

    cache_mem #(.WAYS(WAYS), .AW(SET_AW), .W(TAG_W + 1)) tags (
        .clk(clk),
        .raddr(rd_set),
        .waddr(set),
        .we(tag_we),
        .wdata(tag_wdata),
        .cache_mem_q(tag_q)
    );

    (* ramstyle = "no_rw_check" *)
    row_t row[SETS];
    row_t row_raw;
    always_ff @(posedge clk) begin
        row_raw <= row[rd_set];
        if (row_we)
            row[set] <= row_wdata;
    end
`ifdef VERILATOR
    logic row_rw;
    row_t row_wq;
    always_ff @(posedge clk) begin
        row_rw <= row_we && set == rd_set;
        row_wq <= row_wdata;
    end
    always_comb row_q = row_rw ? ~row_wq : row_raw;
`else
    always_comb row_q = row_raw;
`endif

    always_ff @(posedge clk) begin
        for (int b = 0; b < 4; b++)
            lane_fwd[b] <= lane_waddr == {rd_set, rd_off} ? lane_we[b] : '0;
        lane_fwd_data <= lane_wdata;
        tag_fwd <= set == rd_set ? tag_we : '0;
        tag_fwd_data <= tag_wdata;
        row_fwd <= row_we && set == rd_set;
        row_fwd_data <= row_wdata;
    end

    always_comb begin
        for (int w = 0; w < WAYS; w++) begin
            for (int b = 0; b < 4; b++)
                lane_v[w][8*b+:8] = lane_fwd[b][w] ? lane_fwd_data[b]
                                                   : lane_q[b][w];
            tag_v[w] = tag_fwd[w] ? tag_fwd_data : tag_q[w];
        end
        row_v = row_fwd ? row_fwd_data : row_q;
    end

    logic [WAYS-1:0] hit;
    logic [WAY_AW-1:0] hit_way, oldest;
    always_comb begin
        hit_way = '0;
        oldest = '0;
        for (int w = 0; w < WAYS; w++) begin
            hit[w] = tag_v[w].valid && tag_v[w].tag == dph_tag;
            if (hit[w])
                hit_way = WAY_AW'(w);
            if (row_v.age[w] == WAY_AW'(WAYS - 1))
                oldest = WAY_AW'(w);
        end
        cache_rdata = '0;
        for (int w = 0; w < WAYS; w++)
            if (hit[w])
                cache_rdata |= lane_v[w];
        cache_ready = !dph || (state == S_RUN && |hit);
    end

    always_comb begin
        lane_waddr = {set, state == S_RUN ? dph_off : n};
        for (int b = 0; b < 4; b++) begin
            if (state == S_RUN) begin
                lane_we[b] = dph && dph_write && dph_strb[b] ? hit : '0;
                lane_wdata[b] = wdata[8*b+:8];
            end else begin
                lane_we[b] = '0;
                lane_we[b][way] = state == S_CP;
                lane_wdata[b] = line_rdata[32*n+8*b+:8];
            end
        end

        tag_we = '0;
        tag_wdata = '{valid: 1'b1, tag: dph_tag};
        row_we = 1'b0;
        row_wdata = row_v;
        case (state)
            S_RUN: begin
                /* The way used becomes the youngest, and those younger
                 * than it age by one. A store makes it dirty. */
                row_we = dph && |hit;
                for (int w = 0; w < WAYS; w++)
                    row_wdata.age[w] = WAY_AW'(w) == hit_way ? '0
                                     : row_v.age[w] < row_v.age[hit_way]
                                         ? row_v.age[w] + 1'b1
                                         : row_v.age[w];
                if (dph_write)
                    row_wdata.dirty = row_v.dirty | hit;
            end
            S_EMPTY: begin
                tag_we = '1;
                tag_wdata = '0;
                row_we = 1'b1;
                row_wdata.dirty = '0;
                for (int w = 0; w < WAYS; w++)
                    row_wdata.age[w] = WAY_AW'(WAYS - 1 - w);
            end
            S_CP: begin
                tag_we[way] = n == 2'd3;
                row_we = n == 2'd3;
                row_wdata.dirty[way] = 1'b0;
            end
            default: ;
        endcase
    end

    always_comb begin
        cache_line_we = state == S_WO;
        cache_line_addr = {state == S_WO ? vic_tag : dph_tag, set};
    end

    /* Loads and stores that were answered with no wait, and those that
     * had to fetch their line. Nothing reads them but a test bench. */
    logic [31:0] hits /*verilator public_flat_rd*/;
    logic [31:0] misses /*verilator public_flat_rd*/;
    logic missed;

    /* The walk ends with walk_set back at zero. */
    logic set_last;
    always_comb set_last = walk_set == SET_AW'(SETS - 1);

    initial begin
        state = S_EMPTY;
        walk_set = '0;
        cache_line_pend = 1'b0;
        hits = '0;
        misses = '0;
        missed = 1'b0;
    end
    always_ff @(posedge clk) begin
        case (state)
            S_EMPTY: begin
                walk_set <= walk_set + 1'b1;
                if (set_last)
                    state <= S_REPLAY;
            end
            S_REPLAY: state <= S_RUN;
            S_RUN: begin
                if (dph && hit == '0) begin
                    misses <= misses + 32'd1;
                    missed <= 1'b1;
                    state <= S_TAG;
                end else if (dph) begin
                    if (!missed)
                        hits <= hits + 32'd1;
                    missed <= 1'b0;
                end
            end
            S_TAG: begin
                n <= 2'd1;
                state <= S_VIC;
            end
            S_VIC: begin
                way <= oldest;
                vic_tag <= tag_v[oldest].tag;
                cache_line_wdata <= {lane_v[oldest], cache_line_wdata[127:32]};
                n <= 2'd2;
                if (tag_v[oldest].valid && row_v.dirty[oldest])
                    state <= S_RD;
                else
                    state <= S_FILL;
            end
            S_RD: begin
                cache_line_wdata <= {lane_v[way], cache_line_wdata[127:32]};
                n <= n + 2'd1;
                if (n == 2'd0)
                    state <= S_WO;
            end
            S_WO, S_FILL: begin
                if (!cache_line_pend)
                    cache_line_pend <= !line_done;
                else if (line_done) begin
                    cache_line_pend <= 1'b0;
                    n <= 2'd0;
                    state <= state == S_FILL ? S_CP : S_FILL;
                end
            end
            S_CP: begin
                n <= n + 2'd1;
                if (n == 2'd3)
                    state <= S_REPLAY;
            end
            default: ;
        endcase
        if (!rst_n) begin
            state <= S_EMPTY;
            walk_set <= '0;
            cache_line_pend <= 1'b0;
            missed <= 1'b0;
        end
    end

endmodule
