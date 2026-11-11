/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_COM_RLN_H_
#define _CORE_COM_RLN_H_

/* Readline-like line editor.
 */

#include <stddef.h>
#include <stdint.h>
#include "core/com/com.h"
#include "core/sys/sst.h"
#include <stdbool.h>

/* Main events
 */

void rln_init(void);
void rln_task(void);
void rln_run(void);
void rln_stop(void);

// Timeout is true when input is idle too long.
// Requesting a timeout of 0 ms will disable the idle timer.
typedef void (*rln_read_callback_t)(bool timeout, const char *buf);

// Prepare the line editor. The rln module can read entire
// lines of input with editing on ANSI terminals.
void rln_read_line(rln_read_callback_t callback);

// Read a line typed by a script, such as the monitor's UPLOAD chunk headers:
// no terminal is asked anything, the line is laid out with no prompt column,
// and it is not recorded in history. The callback gets a timeout after
// timeout_ms with no input.
void rln_read_line_script(rln_read_callback_t callback, uint32_t timeout_ms);

// Read a line that is not recorded in history (e.g. a YES/no confirmation),
// keeping the normal (disabled) idle timeout.
void rln_read_line_no_history(rln_read_callback_t callback);

// The source whose line end finished the line, for the callback to read the
// bytes that follow from that source alone. COM_SOURCE_ANY when a poke, a
// flush or the idle timeout finished it.
com_source_t rln_line_source(void);

// Give up a read in progress: the callback never fires.
void rln_read_cancel(void);

// Finish a read now, without waiting for the terminal: the items typed
// before it answered are taken first, then the line is handed over through
// the callback. Returns false when no read was in progress, or when the line
// is still empty.
bool rln_read_flush(void);

// 6502 applications may configure the max length
void rln_set_max_length(uint8_t v);
uint8_t rln_get_max_length(void);

// Caps mode: 0=normal, 1=all caps, 2=invert
void rln_set_caps(uint8_t v);
uint8_t rln_get_caps(void);

// Suppress the line-terminating newline on completion (0=off, 1=on), so
// field input on the last line keeps the cursor on its row. Persists
// across reads; reset on stop().
void rln_set_suppress_nl(uint8_t v);
uint8_t rln_get_suppress_nl(void);

// Forget the line end a source last sent, which normally survives a read,
// because the next client on that source is a different terminal. Takes a
// com_source_t.
void rln_forget_source(unsigned src);

// Inject a sequence of input bytes into the active readline. CR ends
// the line normally; any other C0 control byte (0x00-0x1F) except ESC
// (which begins a CSI sequence) and CAN (0x18, which aborts one) also
// finishes the line, without adding to history. Controls other than CR
// echo as caret notation (^@..^_, 0x03 as ^C) when rln_max_length is at
// least 2, without being inserted. Poked bytes are dispatched in overwrite
// mode. An escape sequence still open at the end of the string is dropped.
// A control byte poked before the terminal answered takes the items typed
// before it first, without waiting for the answer.
void rln_poke(const char *str);

// 6502 API entry points.
bool rln_api_lastkey(void);
bool rln_api_peek(void);
bool rln_api_poke(void);

/* The line being edited. std.c's stdin bridge points into it, and a savestate
 * rebuilds that pointer by calling this rather than carrying it. */
#define RLN_LINE_MAX 256
const char *rln_line(void);

/* The reader is a function pointer, so it is saved as a token. A software
 * machine has exactly one, std.c's stdin bridge, and rebuilding it by calling
 * rln_read_line would ask the terminals again and wipe the line the blob
 * just restored.
 *
 * The idle deadline is machine time, so it is carried whole rather than
 * re-armed. It means the same thing in the machine that loads the blob as in
 * the one that made it. */
#define RLN_SST_SIZE 2631
void rln_sst_save(sst_cursor_t *c, unsigned flags);
bool rln_sst_load(sst_cursor_t *c, unsigned flags);

#define RLN_DRIVER DRIVER(                                  \
    INIT(rln_init)                                          \
    TASK_FATFS(rln_task)                                    \
    RUN(rln_run)                                            \
    STOP(rln_stop)                                          \
    BREAK(rln_stop)                                         \
    SST(RLN_, 4, RLN_SST_SIZE, rln_sst_save, rln_sst_load))

#endif /* _CORE_COM_RLN_H_ */
