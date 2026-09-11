/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * bus.c calls dbg_at_instruction on the opcode fetch cycle (W65C02_SYNC) of the
 * instruction at pc, so a stop leaves the registers in their pre-instruction
 * state. That fetch cycle has already run, so a resume continues into the rest
 * of the instruction rather than fetching it again, which is why a breakpoint
 * never re-triggers on itself.
 */

#include "core/dap/dbg.h"
#include <stdatomic.h>
#include <string.h>

static bool g_active;
static bool g_stopped;
static int g_stop_reason;
static uint16_t g_stop_pc;
static bool g_stop_at_entry;

static dbg_step_t g_step;
static uint8_t g_cur_sp;
static uint8_t g_stop_sp;
static int g_step_line; /* 0 = no line known at the step start */
static const char *g_step_file;
static uint8_t g_step_sp;

static atomic_bool g_pause_req;
static bool g_break_req;

static bool (*g_break_filter)(uint16_t pc);

int dbg_watch_armed;
static bool g_data_pending;
static uint16_t g_data_addr;

static uint8_t g_bp[0x10000 / 8];

static void (*g_stopped_cb)(int reason, uint16_t pc);
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
    g_stop_sp = g_cur_sp; /* a step issued after this stop reads it as its call depth */
    g_step = DBG_STEP_NONE;
}

void dbg_set_stopped_cb(void (*cb)(int reason, uint16_t pc)) { g_stopped_cb = cb; }
void dbg_set_line_lookup(bool (*cb)(uint16_t, const char **, int *)) { g_line_lookup = cb; }

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
    g_step_sp = g_stop_sp;
    g_step_line = 0;
    g_step_file = NULL;
    if (g_line_lookup)
        g_line_lookup(g_stop_pc, &g_step_file, &g_step_line);
    g_stopped = false;
}

/* JSR pushes a return address, so a called subroutine runs with the 6502 SP
 * below the value it had at the step start, and RTS raises it back. Stepping
 * out and stepping over both read the call depth from that. */
static bool step_should_stop(uint16_t pc, uint8_t sp)
{
    if (g_step == DBG_STEP_INSTR)
        return true;
    if (g_step == DBG_STEP_LINE_OUT)
        return sp > g_step_sp;
    /* Without a line table, or without a start line because the program was
     * built without debug info, a line step has only call depth to go on: OVER
     * still runs callees to completion, while INTO becomes one instruction. */
    if (!g_line_lookup || g_step_line == 0)
    {
        if (g_step == DBG_STEP_LINE_OVER)
            return sp >= g_step_sp;
        return true;
    }
    if (g_step == DBG_STEP_LINE_OVER && sp < g_step_sp)
        return false;
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
    if (g_data_pending)
    {
        g_data_pending = false;
        enter_stop(DBG_REASON_DATA, pc);
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
    if (g_break_req)
    {
        enter_stop(DBG_REASON_BREAKPOINT, pc);
        return true;
    }
    /* Breakpoints are tested even in the middle of a step, so one inside a
     * stepped-over call still fires. The filter runs only after the bitmap has
     * matched, so an address with no breakpoint costs a single bit test. */
    if (bp_test(pc) && (!g_break_filter || g_break_filter(pc)))
    {
        enter_stop(DBG_REASON_BREAKPOINT, pc);
        return true;
    }
    return false;
}

void dbg_set_break_filter(bool (*cb)(uint16_t pc)) { g_break_filter = cb; }

void dbg_note_data_stop(uint16_t addr) { g_data_pending = true; g_data_addr = addr; }
uint16_t dbg_data_stop_addr(void) { return g_data_addr; }

static void (*g_watch_cb)(uint16_t addr, uint8_t val, bool is_write);
void dbg_set_watch_cb(void (*cb)(uint16_t addr, uint8_t val, bool is_write)) { g_watch_cb = cb; }
void dbg_watch_access(uint16_t addr, uint8_t val, bool is_write)
{
    if (g_watch_cb)
        g_watch_cb(addr, val, is_write);
}
