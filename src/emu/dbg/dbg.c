/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Debugger core: the one run/stop/step + address-breakpoint engine, shared by
 * the cppdap adapter (dap.cpp) and the on-screen chips debugger (dbgui.cc).
 *
 * Inert until dbg_set_active(true): sys.c's CPU loop only consults it when
 * active, so a normal run is byte-for-byte unaffected. All state changes run on
 * the emulation (main) thread — the cppdap reader thread marshals its requests
 * to the main loop — EXCEPT dbg_request_pause(), which is a lone atomic flag a
 * DAP thread may set at any time.
 *
 * Stop semantics: dbg_at_instruction() is called from the tick loop right after
 * the opcode-fetch cycle (M6502_SYNC) of the instruction at pc, i.e. before that
 * instruction's effect cycles run. Stopping there means PC=pc with the registers
 * in their pre-instruction state; resuming completes the instruction. A given
 * address re-evaluates every time it is fetched, so a breakpoint inside a loop
 * re-triggers without a special "skip the current breakpoint" dance.
 */

#include "emu/dbg/dbg.h"
#include <stdatomic.h>
#include <string.h>

static bool g_active;
static bool g_stopped;
static int g_stop_reason;
static uint16_t g_stop_pc;
static bool g_stop_at_entry;

static dbg_step_t g_step;
static uint8_t g_cur_sp;       /* SP at the current instruction boundary */
static uint8_t g_stop_sp;      /* SP captured at the last stop */
static int g_step_line;        /* source line at the step start (0 = unknown) */
static const char *g_step_file; /* source file at the step start */
static uint8_t g_step_sp;      /* SP at the step start (call-depth reference) */

static atomic_bool g_pause_req;
static bool g_break_req;

/* 64Kbit address-breakpoint bitmap (1 bit per 6502 address). */
static uint8_t g_bp[0x10000 / 8];

static void (*g_stopped_cb)(int reason, uint16_t pc);
/* addr -> {file,line} for source-level stepping; set by the DAP adapter once the
 * program's line table is loaded. NULL -> line steps degrade to instruction steps. */
static bool (*g_line_lookup)(uint16_t addr, const char **file, int *line);

void dbg_set_active(bool on) { g_active = on; }
bool dbg_is_active(void) { return g_active; }

void dbg_request_pause(void) { atomic_store(&g_pause_req, true); }
void dbg_request_break(void) { g_break_req = true; }

static inline bool bp_test(uint16_t a) { return (g_bp[a >> 3] >> (a & 7)) & 1u; }

void dbg_add_breakpoint(uint16_t a) { g_bp[a >> 3] |= (uint8_t)(1u << (a & 7)); }
void dbg_remove_breakpoint(uint16_t a) { g_bp[a >> 3] &= (uint8_t)~(1u << (a & 7)); }
void dbg_clear_breakpoints(void) { memset(g_bp, 0, sizeof g_bp); }
bool dbg_has_breakpoint(uint16_t a) { return bp_test(a); }

bool dbg_is_stopped(void) { return g_stopped; }
int dbg_stop_reason(void) { return g_stop_reason; }
uint16_t dbg_stop_pc(void) { return g_stop_pc; }
void dbg_stop_at_entry(void) { g_stop_at_entry = true; }

void dbg_note_stop(uint16_t pc)
{
    g_stopped = true;
    g_stop_reason = DBG_REASON_PAUSE;
    g_stop_pc = pc;
    g_step = DBG_STEP_NONE;
}

void dbg_set_stopped_cb(void (*cb)(int reason, uint16_t pc)) { g_stopped_cb = cb; }
void dbg_set_line_lookup(bool (*cb)(uint16_t, const char **, int *)) { g_line_lookup = cb; }

/* Program segment table, pushed by the DAP launch handler from the loaded linker
 * output, read by the ImGui memory map. Plain metadata storage (no execution
 * role) — main-thread only, like the line lookup. */
static dbg_segment_t g_segments[DBG_MAX_SEGMENTS];
static int g_nsegments;
static unsigned g_seg_generation;

void dbg_set_segments(const dbg_segment_t *segs, int count)
{
    if (count < 0)
        count = 0;
    if (count > DBG_MAX_SEGMENTS)
        count = DBG_MAX_SEGMENTS;
    if (segs && count)
        memcpy(g_segments, segs, (size_t)count * sizeof *g_segments);
    g_nsegments = count;
    g_seg_generation++;
}

int dbg_get_segments(const dbg_segment_t **out)
{
    if (out)
        *out = g_segments;
    return g_nsegments;
}

unsigned dbg_segments_generation(void) { return g_seg_generation; }

static void enter_stop(int reason, uint16_t pc)
{
    g_stopped = true;
    g_stop_reason = reason;
    g_stop_pc = pc;
    g_stop_sp = g_cur_sp;
    g_step = DBG_STEP_NONE;
    atomic_store(&g_pause_req, false);
    g_break_req = false;
    if (g_stopped_cb)
        g_stopped_cb(reason, pc);
}

void dbg_continue(void)
{
    g_step = DBG_STEP_NONE;
    g_stopped = false;
}

void dbg_step(dbg_step_t kind)
{
    if (!g_stopped)
        return;
    g_step = kind;
    /* Snapshot the call-depth (hardware SP, used by JSR/RTS) and source line at
     * the start, so line/over/out can tell when we've moved on. */
    g_step_sp = g_stop_sp;
    g_step_line = 0;
    g_step_file = NULL;
    if (g_line_lookup)
        g_line_lookup(g_stop_pc, &g_step_file, &g_step_line);
    g_stopped = false;
}

/* Should a pending step stop before the instruction at pc (SP = sp)? */
static bool step_should_stop(uint16_t pc, uint8_t sp)
{
    /* Plain instruction step: stop at the next boundary. */
    if (g_step == DBG_STEP_INSTR)
        return true;
    if (g_step == DBG_STEP_LINE_OUT)
        return sp > g_step_sp; /* returned past the starting frame (RTS unwinds SP) */
    /* OVER/INTO want source lines. With no line table or no known start line (a
     * program built without -g), INTO degrades to a single instruction step, but
     * OVER still steps over calls by call-depth: run straight through anything
     * deeper than the start frame (a JSR lowered SP by 2), stopping once SP is
     * back at the start depth — so Step Over doesn't descend into JSR callees. */
    if (!g_line_lookup || g_step_line == 0)
    {
        if (g_step == DBG_STEP_LINE_OVER)
            return sp >= g_step_sp;
        return true; /* INTO (or any non-OVER line step): single instruction */
    }
    /* OVER: while inside a deeper call (JSR pushed a return address, so SP fell
     * below the start), run straight through it. */
    if (g_step == DBG_STEP_LINE_OVER && sp < g_step_sp)
        return false;
    /* OVER (same/shallower depth) or INTO: stop at the next mapped line that
     * differs from the start line. */
    const char *f = NULL;
    int l = 0;
    if (g_line_lookup(pc, &f, &l) && l != 0 &&
        (l != g_step_line || f != g_step_file))
        return true;
    return false;
}

bool dbg_at_instruction(uint16_t pc, uint8_t sp)
{
    g_cur_sp = sp;

    if (atomic_load(&g_pause_req))
    {
        enter_stop(DBG_REASON_PAUSE, pc);
        return true;
    }
    if (g_stop_at_entry)
    {
        g_stop_at_entry = false;
        enter_stop(DBG_REASON_ENTRY, pc);
        return true;
    }
    if (g_step != DBG_STEP_NONE && step_should_stop(pc, sp))
    {
        enter_stop(DBG_REASON_STEP, pc);
        return true;
    }
    /* Breakpoints fire even mid-step (e.g. a bp inside a stepped-over call). */
    if (g_break_req || bp_test(pc))
    {
        enter_stop(DBG_REASON_BREAKPOINT, pc);
        return true;
    }
    return false;
}
