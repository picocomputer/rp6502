/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_TERM_VTERM_MOUSE_H_
#define _CORE_TERM_VTERM_MOUSE_H_

/* The console's mouse: the terminal's share of the tablet (core/hid/tablet.h),
 * on every machine that has one. It owns the tablet's hooks. It claims the
 * tablet while mode 0 shows the terminal (vterm_placement), so the pointer
 * is the arrow there with no program on the tablet. It claims the wheel
 * (tablet_wheel_claimed) only while a program asks for the mouse's events
 * that tell it: with no scrollback, the wheel does nothing else in the
 * terminal, and a page around the machine keeps it. It acts on contact 0
 * where that is over mode 0's cells:
 *
 * - a press goes to the terminal when it is over mode 0's cells and the
 *   tablet was shared as the press came (tablet_shared, true with no
 *   program on it), and the decision holds to the release, so a drag that
 *   leaves mode 0 keeps going to the terminal;
 * - with nothing pressed, motion and the wheel go to the terminal while
 *   contact 0 is there, over mode 0's cells, and the tablet is shared. A
 *   contact that has gone, a pointer that left or a finger lifted, goes
 *   nowhere.
 *
 * tablet_on_contact only queues what changed, with the share bit and the
 * keyboard's modifiers as they were then, from RAM: every press and release
 * is kept, and motion between them is merged. vterm_mouse_task takes the
 * queue, decides who acts, and acts for the terminal, as xterm does. While
 * a program has a mouse mode set (term.h's mouse_event), what the terminal
 * takes is the program's, and the task types its reports (vtmouse.h) in
 * order with the keys; only a change with Shift held is the terminal's, and
 * that is never told:
 *
 * - the left button selects: a drag selects characters, a double click
 *   words and a triple click lines, and a drag after them goes by words or
 *   by lines. A click with no drag selects nothing, which ends the
 *   selection. The selection is on the screen showing, of the grid
 *   showing, and selecting there moves it there (vterm_select);
 * - a press of the left button on the line the line editor is editing, as
 *   it is laid out at that moment (rln_index_at), selects in the line
 *   instead (core/com/rln.h), and its drag stays in the line. A press
 *   anywhere else is the primary's, even one a drag then takes into the
 *   line. Starting either selection ends the other's highlight. With a
 *   mouse mode set, Shift with the left button selects as the left button
 *   alone does, and only the primary;
 * - the right button, and Shift with the left while no mouse mode is set,
 *   extend the selection from its nearer end, in the unit it was made in;
 * - the release that ends a drag fills the primary (clip.h) with what is
 *   selected;
 * - the middle button pastes the primary as it comes up.
 *
 * With no mouse mode set and alternate scroll (?1007) set, the wheel over
 * the alternate screen presses the cursor keys, as xterm's.
 *
 * Under highlight tracking (?1001) a press of the left button is told and
 * waits for the program's answer (vterm_mouse_track) until it comes up; an
 * answer that starts the highlight makes the drag highlight from where the
 * program said, as a drag selects. A press that goes on from the last drag
 * to end, as xterm's TrackDown counts clicks, highlights at once, by words,
 * lines or characters, and is not told. The release tells what was
 * highlighted, which goes then, and fills the primary. Under the DEC locator
 * (TERM_MOUSE_LOCATOR) the reports are DECLRP, of the events the program
 * chose (term_locator_t), and leaving its filter rectangle. The focus
 * reports (?1004) come from the host (vterm_focus).
 *
 * Clicks are counted on the machine's clock (timer.h), and the gesture is
 * saved with the machine, so a load and runahead count them alike. */

#include "core/sys/ring.h"
#include "core/sys/sst.h"
#include <stdbool.h>
#include <stdint.h>

/* A change of contact 0, as the task takes it. */
typedef struct
{
    int16_t x, y;     // canvas pixels, where it is after the change
    uint8_t flags;    // its TABLET_FLAG_ after the change
    uint8_t modifier; // the keyboard's KEYBOARD_MODIFIER_ as it came
    int8_t wheel;     // what the wheel moved by, with any merged into it
    int8_t pan;       // the same for sideways scrolling
    bool edge;        // a button or hover went up or down: nothing merges into it
    bool moved;       // the user moved it
    bool shared;      // the program shared the tablet (tablet_shared) as it came
} vterm_mouse_event_t;

/* The changes the task has yet to take. A change with no edge merges into
 * the newest when that has none either. A full queue merges every change
 * into the newest, edges included, so the flags the task ends on are
 * always contact 0's. */
#define VTERM_MOUSE_QUEUE 8
typedef RING(vterm_mouse_event_t, VTERM_MOUSE_QUEUE) vterm_mouse_queue_t;
extern vterm_mouse_queue_t vterm_mouse_queue;

/* Whether the terminal took the last change the task took, by the rule
 * above: a release is its press's. */
bool vterm_mouse_acts(void);

/* The host's window gained the focus (true) or lost it. A host that can
 * lose it (host_focusable) says so on every change, or every frame; the
 * task tells a program that asked (?1004) of a change, in order with the
 * keys. */
void vterm_focus(bool focused);

/* ---- what the console's terminal asks of it -----------------------------
 *
 * The console's terminal (term.c) calls these as a program's sequences
 * come. A chip with no console mouse, the Pico VGA, has them do nothing:
 * the RIA's terminal, which follows its, is the one with the mouse. */

/* Highlight tracking's answer, CSI func ; col ; row ; first ; last T, as
 * xterm reads it: col and row are where the highlight starts, from 0, and
 * the drag is held between rows first and last of the screen. It counts
 * only while a press waits for it. func 0 declines, and the press then
 * goes on as a normal event. */
void vterm_mouse_track(int func, int col, int row, int first, int last);

/* DECRQLP: the locator's position is told, unavailable when the locator is
 * off or not over mode 0. As DEC's, a request while the filter rectangle is
 * armed turns the locator off first. As xterm's, the answer of a one-shot
 * locator (DECELR 2) turns off the mouse events, whatever is set, and so
 * does one after it went off, until a DECELR 1. An answer waits for room,
 * after those asked for before it. */
void vterm_mouse_locate(void);

/* DECEFR: the filter rectangle in the console's terminal's locator
 * settings (term_locator_t) is armed, an edge left out where the locator is
 * now, and each held to xterm's limits: in cells, the row and column before
 * the last. One the locator is already outside is
 * told at once, as is one with an edge given while the locator is
 * unavailable. */
void vterm_mouse_filter(void);

void vterm_mouse_task(void);
void vterm_mouse_reset(void);
extern const sst_table_t vterm_mouse_sst;

#define VTERM_MOUSE_DRIVER DRIVER(   \
    TASK_PUMPED(vterm_mouse_task)    \
    RESET(vterm_mouse_reset)         \
    SST("VMOU", vterm_mouse))

#endif /* _CORE_TERM_VTERM_MOUSE_H_ */
