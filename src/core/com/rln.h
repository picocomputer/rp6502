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
// owns it and gives it its memory: every line entered is kept, at its own
// length, until alloc has no room for the next, and then the oldest lines
// go to make the room. A caller starts one with alloc and free and the rest
// zero, and hands it to each read that is to use it.
//
// A line brought back and changed stays changed while the read looks at
// other lines, as the line being typed does. The read's end forgets the
// changes: the line entered becomes the newest, and the lines it was made
// from are as they were entered.
typedef struct
{
    void *(*alloc)(size_t size);
    void (*free)(void *ptr);
    struct rln_history_line *newest;
    struct rln_history_line *oldest;
    struct rln_history_line *at;      // the line showing, NULL for the one being typed
    struct rln_history_line *entered; // the line the last read added, if it added one
    struct rln_history_edit *edits;   // the lines the read has changed
} rln_history_t;

// The line the last read entered is taken out of its history again, as a
// line with a password in it is. Nothing happens when that read added no
// line.
void rln_history_forget(rln_history_t *history);

// What the kill keys take out of a line, for Ctrl-Y to put back: Ctrl-K
// and Ctrl-U the line after and before the cursor, and Ctrl-W,
// Alt-Backspace, Alt-D and Ctrl-Delete a word, or the selection when there
// is one. Kills one after another are one piece, in the order they had in
// the line; any other key between two starts it again. Its caller owns it,
// starts it as zero, and hands it to each read that is to use it, so each
// caller's reads put back only what they took: it lasts for as long as its
// caller keeps it, and a caller empties it by setting its length to 0.
typedef struct
{
    uint8_t len;
    char text[RLN_LINE_MAX - 1];
} rln_kill_t;

// Prepare the line editor. The rln module can read entire
// lines of input with editing on ANSI terminals. The line is recorded in
// history, and the lines before it can be brought back; NULL for a read
// with no history, such as a YES/no confirmation or a program's input.
// What its kill keys take goes to kill; NULL for a read whose kill keys
// only delete, and whose Ctrl-Y does nothing.
// A read with history that lays its line out asks every terminal for
// bracketed paste (?2004), and turns it off again when the read ends.
//
// A paste that comes bracketed, ESC [ 200 ~ to ESC [ 201 ~, is taken up to
// its first line end, and its source's bytes after that, up to the closing
// bracket, are dropped, whatever read they come to. A paste that comes
// without brackets is typing.
void rln_read_line(rln_read_callback_t callback, rln_history_t *history, rln_kill_t *kill);

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
 * console's primary (core/term/vterm_mouse.h): Shift with Left, Right, Home
 * and End selects, and so does the console's mouse inside the line. The
 * line editor shows it as the console's terminal's selection
 * (vterm_select), the part of the line it takes as the line is laid out on
 * the screen, in the line's columns between the margins it was laid out
 * in, once the reply that drew the line has reached that terminal,
 * and ends it before anything of the line is drawn again and when the read
 * ends. One the mouse makes fills the primary when the button comes up,
 * as a selection the mouse makes anywhere on the screen does, and one
 * made with the keys does not. A typed character replaces it, as what a
 * paste types does, Delete and Backspace delete it, the keys that kill a
 * word kill it, Ctrl-Y puts what was killed in its place, and the copy and
 * cut keys act on it. There is one selection at a time, the terminal's: the
 * line's starting ends the primary's highlight, and the primary's starting
 * ends the line's (rln_unselect).
 *
 * The mouse in the line and the copy and cut keys have no bytes, so each
 * waits as an entry in the keyboard's queue (core/hid/typed.h), and the
 * line editor takes it in its place among the keys, after those typed
 * before it and before those typed after, against the line as the keys
 * before it left it, and once the reply to the key before it has gone out.
 * The copy key (core/hid/key.h) puts the selection in the clipboard
 * (core/hid/clip.h) and leaves it selected. A cut key, Shift-Delete,
 * Ctrl-Shift-X and on a Mac Cmd-X, puts it there and deletes it, and what
 * the key would type is dropped. With nothing selected neither does
 * anything. None of them is the last key (rln_api_lastkey).
 *
 * The paste keys are not the line's: what they paste is typed, in its
 * place among the keys, and typing replaces the selection. */

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
 * word, the third the whole line. Until the button drags (rln_mouse_drag),
 * a press by characters is a click, which selects nothing and puts the
 * cursor on the character in overwrite mode and at the nearer gap in
 * insert mode. Its caller has seen to the queue's room (vterm_mouse.c). */
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
 * as it was, and so does a release with no line being read. */
void rln_mouse_release(void);

/* The line's selection ends, its highlight taken off, as the primary's
 * starting ends it, and what the mouse did in the line that the line
 * editor has yet to take is taken back. */
void rln_unselect(void);

/* Whether a line is being read, when the line editor takes its entries
 * from the keyboard's queue, and whether the line has a selection, which
 * may not show yet. */
bool rln_reading(void);
bool rln_selected(void);

/* Nothing waits for the line editor to take: no whole item in any source,
 * and no entry of its own at the front of the keyboard's queue. */
bool rln_idle(void);

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
