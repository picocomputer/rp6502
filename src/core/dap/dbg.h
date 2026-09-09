/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The run/stop/step and breakpoint engine in dbg.c, shared by the DAP adapter
 * (dap.cpp) and the on-screen chips debugger (dbgui.cc). Entry points run on the
 * emulation thread, except dbg_request_pause, which stores an atomic flag and may
 * be called from a DAP reader thread, and dbg_is_stopped, which aud_render also
 * reads on the audio device's thread; the flag it reads is a plain bool, so that
 * read can be a buffer behind.
 */

#ifndef _CORE_DAP_DBG_H_
#define _CORE_DAP_DBG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum
{
    DBG_REASON_ENTRY,
    DBG_REASON_BREAKPOINT,
    DBG_REASON_STEP,
    DBG_REASON_PAUSE,
    DBG_REASON_DATA,
} dbg_reason_t;

typedef enum
{
    DBG_STEP_NONE,
    DBG_STEP_INSTR,
    DBG_STEP_LINE_OVER,
    DBG_STEP_LINE_INTO,
    DBG_STEP_LINE_OUT,
} dbg_step_t;

void dbg_set_active(bool on);
bool dbg_is_active(void);

void dbg_request_pause(void);
void dbg_request_break(void); /* like a pause, but reported as a breakpoint hit */
void dbg_continue(void);
void dbg_step(dbg_step_t kind);
bool dbg_is_stopped(void);
int dbg_stop_reason(void); /* a dbg_reason_t */
uint16_t dbg_stop_pc(void);
void dbg_stop_at_entry(void); /* a one-shot stop at the first instruction */
/* Stop at pc without firing the stopped callback, so that a program exit can be
 * presented as a stop the client did not ask for. */
void dbg_note_stop(uint16_t pc);

void dbg_clear_breakpoints(void);
void dbg_add_breakpoint(uint16_t addr);
void dbg_remove_breakpoint(uint16_t addr);
bool dbg_has_breakpoint(uint16_t addr);

/* Runs after a breakpoint address matches; returning false keeps the machine
 * running, which is how a condition, a hit count, or a logpoint is expressed.
 * NULL, the default, stops on every breakpoint. */
void dbg_set_break_filter(bool (*cb)(uint16_t pc));

/* Watchpoints, which the Debug Adapter Protocol calls data breakpoints. The DAP
 * layer sets dbg_watch_armed to the number of watches, so the bus hook in
 * core/wdc/bus.c costs one branch per cycle when there are none. On a hit the
 * watch callback calls dbg_note_data_stop and the machine stops at the next
 * instruction boundary, by which time the access has completed. */
extern int dbg_watch_armed;
void dbg_note_data_stop(uint16_t data_addr);
uint16_t dbg_data_stop_addr(void);
void dbg_set_watch_cb(void (*cb)(uint16_t addr, uint8_t val, bool is_write));
void dbg_watch_access(uint16_t addr, uint8_t val, bool is_write);

/* One observer only. The on-screen debugger polls dbg_is_stopped instead, which
 * leaves this callback to the DAP adapter. */
void dbg_set_stopped_cb(void (*cb)(int reason, uint16_t pc));

/* Source-level stepping needs this map from address to file and line. Without
 * it, the default, a step into runs as an instruction step, while step over and
 * step out still work from call depth alone. */
void dbg_set_line_lookup(bool (*cb)(uint16_t addr, const char **file, int *line));

/* The loaded program's segments: the allocatable sections of an llvm-mos ELF, or
 * the seg records of a cc65 .dbg. Empty until such a program is launched. */
#define DBG_MAX_SEGMENTS 24
typedef struct
{
    char name[24];
    uint16_t addr;
    uint32_t size;
} dbg_segment_t;
void dbg_set_segments(const dbg_segment_t *segs, int count); /* copies, clamped to DBG_MAX_SEGMENTS */
int dbg_get_segments(const dbg_segment_t **out);
unsigned dbg_segments_generation(void);

/* Called by bus.c on each opcode fetch while the debugger is active. */
bool dbg_at_instruction(uint16_t pc, uint8_t sp);

#endif /* _CORE_DAP_DBG_H_ */
