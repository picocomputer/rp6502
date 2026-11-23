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

/* A key's reply is still going out at the console line's pace, or the read
 * it ended is waiting to be handed its line. */
bool rln_replying(void);
void rln_run(void);
void rln_stop(void);
void rln_reset(void);

// Timeout is true when input is idle too long.
// Requesting a timeout of 0 ms will disable the idle timer.
typedef void (*rln_read_callback_t)(bool timeout, const char *buf);

#define RLN_LINE_MAX 256

// The lines a read can bring back with the up and down keys. Its caller
// owns it and says how many lines it has: `line` is that many, the first of
// them the line being typed, kept while older ones are looked at, and the
// rest the lines entered, newest first. A caller starts one with its lines
// and their count and the rest zero, and hands it to each read that is to
// use it.
typedef struct
{
    char (*line)[RLN_LINE_MAX];
    uint8_t lines;
    uint8_t count; // the lines entered that it holds
    uint8_t pos;   // the line showing, 0 for the one being typed
} rln_history_t;

// Prepare the line editor. The rln module can read entire
// lines of input with editing on ANSI terminals. The line is recorded in
// history, and the lines before it can be brought back; NULL for a read
// with no history, such as a YES/no confirmation or a program's input.
// A read with history that lays its line out asks every terminal for
// bracketed paste (?2004), and turns it off again when the read ends.
//
// A paste that comes bracketed, ESC [ 200 ~ to ESC [ 201 ~, is taken up to
// its first line end, and its source's bytes after that, up to the closing
// bracket, are dropped, whatever read they come to. A paste that comes
// without brackets is typing.
void rln_read_line(rln_read_callback_t callback, rln_history_t *history);

// Read a line typed by a script, such as the monitor's UPLOAD chunk headers:
// the line is laid out plainly, as rln_set_plain does, and it has no
// history. The callback gets a timeout after timeout_ms with no input.
void rln_read_line_script(rln_read_callback_t callback, uint32_t timeout_ms);

// The source whose line end finished the line, for the callback to read the
// bytes that follow from that source alone. COM_SOURCE_ANY when a poke, a
// flush or the idle timeout finished it.
com_source_t rln_line_source(void);

// Give up a read in progress: the callback never fires.
void rln_read_cancel(void);

// Finish a read now, handing the line over through the callback. Returns
// false when no read was in progress, or when the line is still empty.
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

// Plain lines, for a console no one sees laid out, such as an emulator
// printing to a pipe: the line is echoed and edited with backspaces, reprints
// and spaces, with no prompt column, no row math and no escape sequence. The
// host sets it once; it outlives every read and stop. A read that starts
// while the console's terminal is in VT52 mode is plain too.
void rln_set_plain(bool plain);

/* The console's size: a program's own while it pins one, otherwise the
 * screen's showing grid. The line editor lays its lines out at it. */
uint16_t rln_get_term_width(void);
uint16_t rln_get_term_height(void);

/* A non-zero value pins the size, and 0 goes back to the screen's. A pinned
 * size is a third grid of the console's terminal, up to RLN_GRID_ROWS rows,
 * which the line editor and the terminal's answers use. Both return to 0
 * when a program stops. */
#define RLN_GRID_ROWS 255
void rln_set_term_width(uint16_t v);
void rln_set_term_height(uint16_t v);

// The terminal on src went away, and the next one there is a different
// terminal: the line end it last sent, which normally survives a read, and
// what tio holds of an overlong sequence from it are forgotten.
void rln_forget_source(com_source_t src);

// Inject a sequence of input bytes into the active readline. CR ends
// the line normally; any other C0 control byte (0x00-0x1F) except ESC
// (which begins an escape sequence) also finishes the line, without adding
// to history, unless it is CAN (0x18) or SUB (0x1A) inside a sequence, which
// cancels the sequence. Controls other than CR echo as caret notation
// (^@..^_, 0x03 as ^C) when rln_max_length is at least 2, without being
// inserted. Poked bytes are dispatched in overwrite mode, and only poked
// bytes run the editing commands DCH (CSI Ps P), ICH (CSI Ps @) and ECH
// (CSI Ps X). An escape sequence still open at the end of the string is
// dropped.
void rln_poke(const char *str);

/* ----- The line's selection -----
 *
 * The line being edited has a selection of its own, apart from the
 * console's primary (core/term/vterm_mouse.h): Shift with the arrows, Home
 * and End selects, and so does the console's mouse inside the line. The
 * line editor shows it as the console's terminal's selection
 * (vterm_select), the part of the line it takes as the line is laid out on
 * the screen, in the line's columns between the margins it was laid out
 * in, once the reply that drew the line has reached that terminal,
 * and ends it before anything of the line is drawn again and when the read
 * ends. One the mouse makes fills the primary when the button comes up,
 * as a selection the mouse makes anywhere on the screen does, and one
 * made with the keys does not. A typed character replaces it,
 * Delete and Backspace delete it, and the copy, cut and paste keys act on
 * it (rln_copy, rln_cut, rln_paste). There is one selection at a time,
 * the terminal's: the line's starting ends the primary's highlight, and the
 * primary's starting ends the line's (rln_unselect). */

/* The index of the line at row y of the screen showing, column x of that
 * row's own, as the line is laid out now, from the line's first character
 * to the place after its last; -1 for a place outside it, or for a line
 * with no place there: no read, a plain line, a program's own size
 * pinned, or a grid or screen showing that the line was not laid out on. */
int rln_index_at(uint8_t y, uint8_t x);

/* The left button went down at index of the line (rln_index_at), after
 * when the pointer is nearer the gap after that character than the gap
 * before it, its place in its run of clicks as the console's mouse
 * counted it: the first and the fourth select by characters, the second a
 * word, the third the whole line. The line editor takes it after the keys
 * already waiting for it, against the line as those keys left it. Until
 * the button drags (rln_mouse_drag), a press by characters is a click,
 * which selects nothing and puts the cursor on the character in
 * overwrite mode and at the nearer gap in insert mode. */
void rln_mouse_press(uint8_t index, bool after, uint8_t clicks);

/* The button held since that press moved: the selection is from row hy,
 * column hx, the end the drag holds, to row y, column x, the end it moves,
 * as rln_index_at has them. By characters each is a gap, the column of the
 * character after it, as the console's mouse puts them (vterm_mouse.c);
 * by words and lines, hx is the press's character and x the one under the
 * pointer. A place before the line or after it is the line's start or its
 * end. A drag whose press was in a read that has ended selects nothing. */
void rln_mouse_drag(uint8_t hy, uint8_t hx, uint8_t y, uint8_t x);

/* The button held since that press came up: what the press and its drag
 * select, taken as they are, fills the primary (core/hid/clip.h) with the
 * line's characters. A click, which selects nothing, leaves the primary
 * as it was. */
void rln_mouse_release(void);

/* The line's selection ends, its highlight taken off, as the primary's
 * starting ends it. */
void rln_unselect(void);

/* Whether the line has a selection showing, which the copy, cut and
 * paste keys act on. */
bool rln_selected(void);

/* The copy keys (core/hid/key.h), at once: the line's selection goes to
 * the clipboard (core/hid/clip.h), as it is at that moment. False with
 * none, when the copy keys copy the primary. */
bool rln_copy(void);

/* The cut keys, Shift-Delete, Ctrl-Shift-X and on a Mac Cmd-X, and the
 * paste keys, judged at once: with the line's selection showing they are
 * the line's, and true. The line editor takes them after the keys already
 * waiting for it, as it takes the mouse, on the line as those keys left
 * it: a cut puts the selection in the clipboard and deletes it, and a
 * paste deletes it and pastes the clipboard (core/hid/paste.h) in its
 * place. A later one takes the place of one still to take. A paste still
 * to take when the read ends pastes as it would with no read, unless a
 * stop or a break ends it. False with no selection showing, when a cut
 * key is not a chord and types as it would, and a paste pastes at once. */
bool rln_cut(void);
bool rln_paste(void);

// 6502 API entry points.
bool rln_api_lastkey(void);
bool rln_api_peek(void);
bool rln_api_poke(void);

/* The line being edited. console.c's stdin bridge reads the line it was
 * handed from here. */
const char *rln_line(void);

extern const sst_table_t rln_sst;

#define RLN_DRIVER DRIVER(                                  \
    INIT(rln_init)                                          \
    TASK_FATFS(rln_task)                                    \
    RUN(rln_run)                                            \
    STOP(rln_stop)                                          \
    BREAK(rln_stop)                                         \
    RESET(rln_reset)                                        \
    SST("RLN_", rln))

#endif /* _CORE_COM_RLN_H_ */
