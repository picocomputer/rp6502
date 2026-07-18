/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Debugger core (dbg.c) — the one authoritative run/stop/step + address-
 * breakpoint engine, shared by the cppdap adapter (dap.cpp) and the on-screen
 * chips debugger (dbgui.cc). Inert until dbg_set_active(true): the CPU loop in
 * cpu.c only consults it when active, so a normal run is unaffected. All control
 * runs on the main (emulation) thread EXCEPT dbg_request_pause, which a DAP
 * reader thread may set; it is a lone atomic flag.
 */

#ifndef _EMU_DBG_DBG_H_
#define _EMU_DBG_DBG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum
{
    DBG_REASON_ENTRY,      /* stopped at program entry (stopOnEntry) */
    DBG_REASON_BREAKPOINT, /* hit an address breakpoint */
    DBG_REASON_STEP,       /* a step completed */
    DBG_REASON_PAUSE,      /* host/DAP requested a pause */
    DBG_REASON_DATA,       /* a watchpoint (data breakpoint) tripped */
} dbg_reason_t;

typedef enum
{
    DBG_STEP_NONE,
    DBG_STEP_INSTR,     /* one machine instruction */
    DBG_STEP_LINE_OVER, /* one source line; called subroutines run to return */
    DBG_STEP_LINE_INTO, /* one source line, descending into calls */
    DBG_STEP_LINE_OUT,  /* until the current subroutine returns */
} dbg_step_t;

void dbg_set_active(bool on);
bool dbg_is_active(void);

/* Control surface (continue/step on the main thread; pause may be cross-thread). */
void dbg_request_pause(void);   /* stop at the next instruction (thread-safe) */
void dbg_request_break(void);   /* stop at the next instruction as a breakpoint hit (main thread) */
void dbg_continue(void);        /* resume free-run */
void dbg_step(dbg_step_t kind); /* resume until the step completes, then stop */
bool dbg_is_stopped(void);
int dbg_stop_reason(void);     /* dbg_reason_t of the current stop */
uint16_t dbg_stop_pc(void);    /* instruction address where execution stopped */
void dbg_stop_at_entry(void);  /* arm a one-shot stop at the first instruction */
/* Force the engine into a stopped state at pc (no stopped_cb) — used to present
 * a program exit as a debugger stop so the final screen + state stay inspectable
 * until the client disconnects. */
void dbg_note_stop(uint16_t pc);

/* Address breakpoints (the source-line mapper resolves lines to addresses). */
void dbg_clear_breakpoints(void);
void dbg_add_breakpoint(uint16_t addr);
void dbg_remove_breakpoint(uint16_t addr);
bool dbg_has_breakpoint(uint16_t addr);

/* Optional gate run (main thread) after a breakpoint's bitmap bit matches: return
 * true to stop, false to keep running (condition unmet / logpoint). NULL clears. */
void dbg_set_break_filter(bool (*cb)(uint16_t pc));

/* Watchpoints (data breakpoints). The bus write/read hook checks dbg_watch_armed
 * (0 = none, skip) and, on a hit, calls dbg_note_data_stop(addr) to stop at the
 * next instruction boundary with reason DATA. dbg_watch_armed is owned by the DAP
 * layer (main thread); the count keeps the per-cycle bus hook a single branch. */
extern int dbg_watch_armed;
void dbg_note_data_stop(uint16_t data_addr);
uint16_t dbg_data_stop_addr(void);
/* The DAP layer registers the watch scanner; the bus hook (cpu.c) invokes it only
 * when dbg_watch_armed != 0. is_write distinguishes a store from a load. */
void dbg_set_watch_cb(void (*cb)(uint16_t addr, uint8_t val, bool is_write));
void dbg_watch_access(uint16_t addr, uint8_t val, bool is_write);

/* Fired on the main thread when execution halts (DAP adapter -> StoppedEvent).
 * The ImGui view polls dbg_is_stopped() instead, so one observer is enough. */
void dbg_set_stopped_cb(void (*cb)(int reason, uint16_t pc));

/* addr -> {file,line} for source-level stepping; set by the DAP adapter once the
 * DWARF line table is loaded. NULL (default) -> line steps act as instr steps. */
void dbg_set_line_lookup(bool (*cb)(uint16_t addr, const char **file, int *line));

/* Program segments from the loaded linker output: the llvm-mos ELF's allocatable
 * sections, or the cc65 .dbg seg records. The DAP launch handler pushes them after
 * loading the source map; the ImGui memory map reads them to show each segment's
 * load address + size. Empty until a program with debug info is launched. */
#define DBG_MAX_SEGMENTS 24
typedef struct
{
    char name[24];
    uint16_t addr;
    uint32_t size;
} dbg_segment_t;
void dbg_set_segments(const dbg_segment_t *segs, int count); /* copies; clamps to DBG_MAX_SEGMENTS */
int dbg_get_segments(const dbg_segment_t **out);             /* count; *out -> the table */
unsigned dbg_segments_generation(void);                      /* bumps on each set (cheap change check) */

/* Called by cpu.c at each instruction fetch (M6502_SYNC) while active. Returns
 * true if the machine must stop BEFORE running the instruction's effect at pc. */
bool dbg_at_instruction(uint16_t pc, uint8_t sp);

#endif /* _EMU_DBG_DBG_H_ */
