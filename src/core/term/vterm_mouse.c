/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/term/vterm_mouse.h"
#include "core/com/rln.h"
#include "core/hid/clip.h"
#include "core/hid/keyboard.h"
#include "core/hid/tablet.h"
#include "core/hid/typed.h"
#include "core/hid/usage.h"
#include "core/hid/vtkey.h"
#include "core/hid/vtmouse.h"
#include "core/sys/timer.h"
#include "core/term/cell.h"
#include "core/term/vterm.h"
#include "machine.h"
#include <assert.h>
#include <stddef.h>
#include <stdlib.h>

vterm_mouse_queue_t vterm_mouse_queue;

/* Contact 0's flags as the task last took them, and whether the terminal
 * took the last change: decided at a press and held to the release, and
 * otherwise at every change. Whether the program that asked for the
 * mouse's events took it is decided with it. */
static uint8_t vterm_mouse_flags;
static bool vterm_mouse_acting;
static bool vterm_mouse_reporting;

/* Where the last report put the pointer: its cell, or its pixel in the
 * pixel form, from which motion is told only when it moves on, as xterm's
 * mouse_row and mouse_col. */
static uint16_t vterm_mouse_sent_x, vterm_mouse_sent_y;

/* Where the last change the task took left contact 0, in canvas pixels,
 * and whether it is there at all (VTERM_MOUSE_PRESENT): where the DEC
 * locator is. */
static int16_t vterm_mouse_x, vterm_mouse_y;
static bool vterm_mouse_here;

/* Highlight tracking (?1001), as xterm's: off; a press of the left button
 * told, and the program's answer (vterm_mouse_track) awaited until it
 * comes up; or the drag highlighting from where the answer said, held to
 * rows first to last of the screen. */
#define VTERM_MOUSE_TRACK_OFF 0
#define VTERM_MOUSE_TRACK_WAIT 1
#define VTERM_MOUSE_TRACK_ON 2
static uint8_t vterm_mouse_tracking;
static uint8_t vterm_mouse_track_first, vterm_mouse_track_last;

/* The rows a highlight is held to as xterm's are before any are set: all
 * of them, as a row past the screen's last is held to its last. */
#define VTERM_MOUSE_TRACK_ALL (TERM_MAX_HEIGHT - 1)

/* The DEC locator reports the terminal asked for and the task has yet to
 * type, waiting for room, oldest first: their events (VTMOUSE_LOCATOR_).
 * Each one's position is where the locator is as it is typed. One asked
 * for while they are full is dropped, as an answer with no room is
 * (com_reply). */
#define VTERM_MOUSE_DUE 4
static uint8_t vterm_mouse_due[VTERM_MOUSE_DUE];
static uint8_t vterm_mouse_dues;

/* Whether the host's window has the focus, and whether the program was last
 * told it has (?1004): a change between them is told. */
static bool vterm_focus_now = true;
static bool vterm_focus_told = true;

/* The buttons, which make a press: hover alone is none. */
#define VTERM_MOUSE_BUTTONS (TABLET_FLAG_LEFT | TABLET_FLAG_RIGHT | TABLET_FLAG_MIDDLE | \
                             TABLET_FLAG_BTN4 | TABLET_FLAG_BTN5)

/* A contact is there while it hovers or presses; one with neither has gone,
 * as a pointer that left or a finger lifted. */
#define VTERM_MOUSE_PRESENT (VTERM_MOUSE_BUTTONS | TABLET_FLAG_HOVER)

/* ---- the gesture --------------------------------------------------------
 *
 * The selecting, as xterm's: a press of the left button selects from where
 * it went down, by characters, and a drag takes the selection to where the
 * pointer is. A second and a third click on the same cell, each within
 * VTERM_MOUSE_CLICK_MS of the last release, select by words and by lines,
 * and a drag after them goes by words and by lines; a fourth and a fifth
 * select by characters, and a sixth by words again, as xterm's clicks go
 * with its default on2Clicks and on3Clicks. The right button, and Shift
 * with the left, extend the selection from its nearer end, in the unit it
 * was made in. The release fills the primary with what is selected. The
 * middle button pastes the primary as it comes up. While a mouse mode is
 * set, the buttons alone are the program's and the terminal takes them
 * with Shift: Shift with the left button then selects as the left alone
 * does, Shift with the right extends and Shift with the middle pastes.
 *
 * By characters, a selection's ends are the gaps between characters, as
 * Windows Terminal's are: the press's character is the selection's start,
 * taken whole as soon as the pointer moves from the press, by a pixel, any
 * way, but not when the press was in its last pixel column and the
 * pointer goes on past it, or its first and the pointer goes back before
 * it; and the end the drag moves is the gap nearest the pointer, so a
 * further character joins once the pointer passes its middle. A press that
 * does not move selects nothing. The pointer is on the row it is over,
 * past the row's text too. An end that extends, or a highlight a program
 * started, holds at its gap.
 *
 * The points are the store's marks (screen.h), on the screen the gesture
 * is on: rows of memory, as the selection is, so output that scrolls the
 * screen during a drag keeps them on their text, and one whose row leaves
 * the screen goes to the edge it left by, as xterm's do. A point keeps its
 * place inside its cell, as the press's must. */

enum
{
    VTERM_MOUSE_CHAR,
    VTERM_MOUSE_WORD,
    VTERM_MOUSE_LINE,
};

/* The unit of each click in a run, as xterm's selectMap with its default
 * on2Clicks and on3Clicks: the run goes round it, its fifth click by
 * characters as its first. */
static const uint8_t vterm_mouse_units[] = {VTERM_MOUSE_CHAR, VTERM_MOUSE_WORD, VTERM_MOUSE_LINE,
                                            VTERM_MOUSE_CHAR};
#define VTERM_MOUSE_CLICKS (sizeof vterm_mouse_units / sizeof *vterm_mouse_units)

/* The console's widest grid. */
#define VTERM_MOUSE_MAX_COLS 80

/* A point of a screen: a row of memory, a column of that row's own, and
 * where in that column's cell, 0 for its left edge (screen.h). */
typedef screen_mark_t vterm_mouse_cell_t;

/* The points the gesture holds, which the store keeps in step as the
 * marks, an array of them. */
#define VTERM_MOUSE_POINTS 6

typedef struct
{
    uint8_t screen;  // the screen it is on: bit 1 the 80-column grid, bit 0 the alternate
    uint8_t button;  // TABLET_FLAG_LEFT or _RIGHT while its drag goes on, else 0
    uint8_t unit;    // VTERM_MOUSE_CHAR, _WORD or _LINE
    uint8_t clicks;  // its place in the left button's run, 1 to VTERM_MOUSE_CLICKS, or 0 before any
    bool left;       // the drag moves the start, xterm's LEFTEXTENSION
    bool clicked;    // the last press was a left click that counts, and no other button went down or up since
    bool line;       // the last press selected in the line editor's line (core/com/rln.h)
    bool ended_left; // the last drag ended was the left button's (vterm_mouse_ended), xterm's lastButton
    bool dragged;    // the pointer has moved since the last press, which then drags
    union
    {
        struct
        {
            vterm_mouse_cell_t ext_start, ext_end;   // what is selected between, as the unit has not widened it
            vterm_mouse_cell_t save_start, save_end; // ext_start and ext_end as the drag began, or the press
            vterm_mouse_cell_t raw;                  // where the run's first click started, xterm's rawPos
            vterm_mouse_cell_t click;                // where the left button last went down, its pixel's place
        };
        vterm_mouse_cell_t points[VTERM_MOUSE_POINTS]; // the same, for the store
    };
    timer_mach_t click_by; // a press before this goes on from the last drag ended, xterm's lastButtonUpTime
} vterm_mouse_gesture_t;
static vterm_mouse_gesture_t vterm_mouse_gesture;
_Static_assert(offsetof(vterm_mouse_gesture_t, click) ==
                       offsetof(vterm_mouse_gesture_t, points) +
                           (VTERM_MOUSE_POINTS - 1) * sizeof(vterm_mouse_cell_t) &&
                   VTERM_MOUSE_POINTS <= 8,
               "the gesture's points are the marks, no more than the store keeps");

/* The screen of a gesture's code. */
static const screen_t *vterm_mouse_screen(uint8_t code)
{
    return &(code >> 1 ? &vterm_80 : &vterm_40)->grid.screen[code & 1];
}

/* The store keeps the gesture's points in step from its first press. */
static void vterm_mouse_mark(void)
{
    vterm_marks = (screen_marks_t){
        .screen = vterm_mouse_gesture.clicks ? vterm_mouse_screen(vterm_mouse_gesture.screen)
                                             : NULL,
        .at = vterm_mouse_gesture.points,
        .n = VTERM_MOUSE_POINTS,
    };
}

/* Whether row of memory m is a row of the height of s, a screen of g. */
static bool vterm_mouse_shown_row(const term_grid_t *g, const screen_t *s, uint8_t m)
{
    for (uint8_t y = 0; y < g->height; y++)
        if (screen_mem(s, y) == m)
            return true;
    return false;
}

/* The counters may be anything that holds no more than the queue. After a
 * first press the gesture's points are on rows of the height of its screen,
 * which the CELL chunk loaded before this one (drivers.h), as the store
 * keeps them; then the store keeps them in step again. */
static bool vterm_mouse_sst_valid(sst_cursor_t *c)
{
    if (RING_LEN(&vterm_mouse_queue) > VTERM_MOUSE_QUEUE)
        return sst_refuse(c, "more mouse changes queued than the queue holds");
    if (vterm_mouse_reporting && !vterm_mouse_acting)
        return sst_refuse(c, "a program told of a change the terminal did not take");
    if (vterm_mouse_tracking > VTERM_MOUSE_TRACK_ON ||
        vterm_mouse_track_first >= TERM_MAX_HEIGHT || vterm_mouse_track_last >= TERM_MAX_HEIGHT ||
        vterm_mouse_dues > VTERM_MOUSE_DUE)
        return sst_refuse(c, "highlight tracking or a locator report out of range");
    for (uint8_t i = 0; i < vterm_mouse_dues; i++)
        if (vterm_mouse_due[i] > VTMOUSE_LOCATOR_OUTSIDE)
            return sst_refuse(c, "a locator report out of range");
    const vterm_mouse_gesture_t *gs = &vterm_mouse_gesture;
    if (gs->screen > 3 || gs->unit > VTERM_MOUSE_LINE || gs->clicks > VTERM_MOUSE_CLICKS ||
        (gs->button & ~(TABLET_FLAG_LEFT | TABLET_FLAG_RIGHT)) || (gs->button && !gs->clicks) ||
        (gs->line && gs->button && gs->button != TABLET_FLAG_LEFT) ||
        (vterm_mouse_tracking == VTERM_MOUSE_TRACK_ON && (gs->button != TABLET_FLAG_LEFT || gs->line)))
        return sst_refuse(c, "a mouse gesture off the console's screens");
    if (gs->clicks)
    {
        const term_grid_t *g = gs->screen >> 1 ? &vterm_80.grid : &vterm_40.grid;
        const screen_t *s = vterm_mouse_screen(gs->screen);
        if (!s->cells)
            return sst_refuse(c, "a mouse gesture on a screen with no cells");
        for (unsigned i = 0; i < VTERM_MOUSE_POINTS; i++)
            if (!vterm_mouse_shown_row(g, s, gs->points[i].row) ||
                gs->points[i].col > VTERM_MOUSE_MAX_COLS || gs->points[i].sub > SCREEN_MARK_SUBS)
                return sst_refuse(c, "a mouse gesture off its screen's rows");
    }
    vterm_mouse_mark();
    return true;
}

#define VTERM_MOUSE_EVENT_ROWS(Y, e)                         \
    Y(I16, e.x) Y(I16, e.y) Y(U8, e.flags) Y(U8, e.modifier) \
    Y(U8, e.wheel) Y(U8, e.pan) Y(BOOL, e.edge)              \
    Y(BOOL, e.moved) Y(BOOL, e.shared)
#define VTERM_MOUSE_CELL_ROWS(Y, p) Y(U8, p.row) Y(U8, p.col) Y(U8, p.sub)
#define VTERM_MOUSE_SST(X)                                                    \
    X(U32, vterm_mouse_queue.idx.wr)                                          \
    X(U32, vterm_mouse_queue.idx.rd)                                          \
    X(STRUCTS, vterm_mouse_queue.buf, VTERM_MOUSE_EVENT_ROWS)                 \
    X(U8, vterm_mouse_gesture.screen)                                         \
    X(U8, vterm_mouse_gesture.button)                                         \
    X(U8, vterm_mouse_gesture.unit)                                           \
    X(U8, vterm_mouse_gesture.clicks)                                         \
    X(BOOL, vterm_mouse_gesture.left)                                         \
    X(BOOL, vterm_mouse_gesture.clicked)                                      \
    X(BOOL, vterm_mouse_gesture.line)                                         \
    X(BOOL, vterm_mouse_gesture.ended_left)                                   \
    X(BOOL, vterm_mouse_gesture.dragged)                                      \
    X(STRUCT, vterm_mouse_gesture.ext_start, VTERM_MOUSE_CELL_ROWS)           \
    X(STRUCT, vterm_mouse_gesture.ext_end, VTERM_MOUSE_CELL_ROWS)             \
    X(STRUCT, vterm_mouse_gesture.save_start, VTERM_MOUSE_CELL_ROWS)          \
    X(STRUCT, vterm_mouse_gesture.save_end, VTERM_MOUSE_CELL_ROWS)            \
    X(STRUCT, vterm_mouse_gesture.raw, VTERM_MOUSE_CELL_ROWS)                 \
    X(STRUCT, vterm_mouse_gesture.click, VTERM_MOUSE_CELL_ROWS)               \
    X(U64, vterm_mouse_gesture.click_by)                                      \
    X(BOOL, vterm_mouse_reporting)                                            \
    X(U16, vterm_mouse_sent_x)                                                \
    X(U16, vterm_mouse_sent_y)                                                \
    X(I16, vterm_mouse_x)                                                     \
    X(I16, vterm_mouse_y)                                                     \
    X(BOOL, vterm_mouse_here)                                                 \
    X(U8, vterm_mouse_tracking)                                               \
    X(U8, vterm_mouse_track_first)                                            \
    X(U8, vterm_mouse_track_last)                                             \
    X(BYTES, vterm_mouse_due)                                                 \
    X(U8, vterm_mouse_dues)                                                   \
    X(BOOL, vterm_focus_told)                                                 \
    X(SKIP, vterm_focus_now, "the host's, which it keeps across a load")      \
    X(U8, vterm_mouse_flags)                                                  \
    X(BOOL, vterm_mouse_acting)                                               \
    X(VALIDATE, vterm_mouse_sst_valid)
SST_TABLE(vterm_mouse, VTERM_MOUSE_SST)

/* A failed load can leave the counters holding more than the queue, and the
 * gesture anywhere. */
void vterm_mouse_reset(void)
{
    RING_RESET(&vterm_mouse_queue);
    vterm_mouse_flags = 0;
    vterm_mouse_acting = false;
    vterm_mouse_reporting = false;
    vterm_mouse_sent_x = vterm_mouse_sent_y = UINT16_MAX;
    vterm_mouse_x = vterm_mouse_y = 0;
    vterm_mouse_here = false;
    vterm_mouse_tracking = VTERM_MOUSE_TRACK_OFF;
    vterm_mouse_track_first = 0;
    vterm_mouse_track_last = VTERM_MOUSE_TRACK_ALL;
    vterm_mouse_dues = 0;
    vterm_focus_told = vterm_focus_now;
    vterm_mouse_gesture = (vterm_mouse_gesture_t){0};
    vterm_mouse_mark();
}

bool tablet_claimed(void)
{
    return vterm_placement.placed;
}

/* With no scrollback, the wheel presses the cursor keys over the alternate
 * screen with alternate scroll (?1007) and no mouse mode, as xterm's. */
static bool vterm_mouse_alt_scrolls(void)
{
    return vterm.alt_scroll && vterm.alt_active && vterm.mouse_event == TERM_MOUSE_OFF;
}

/* The terminal has no scrollback, so the wheel does something here only
 * for a program that asked for the mouse's events that tell it, the normal
 * events to the any events, or with alternate scroll. */
bool tablet_wheel_claimed(void)
{
    const uint8_t ev = vterm.mouse_event;
    return vterm_placement.placed &&
           ((ev >= TERM_MOUSE_NORMAL && ev <= TERM_MOUSE_ANY) || vterm_mouse_alt_scrolls());
}

/* A sum of two deltas, held at the ends of an int8_t. *fits is cleared when
 * it was held. */
static int8_t HOST_IN_RAM_FUNC vterm_mouse_add(int8_t a, int8_t b, bool *fits)
{
    const int sum = a + b;
    if (sum < INT8_MIN || sum > INT8_MAX)
    {
        *fits = false;
        return sum < 0 ? INT8_MIN : INT8_MAX;
    }
    return (int8_t)sum;
}

void HOST_IN_RAM_FUNC tablet_on_contact(uint8_t old_flags, uint8_t flags, int16_t x, int16_t y,
                                        int8_t dwheel, int8_t dpan, bool moved_by_user)
{
    const uint32_t len = RING_LEN(&vterm_mouse_queue);
    const bool edge = flags != old_flags;
    const bool shared = tablet_shared();
    const uint8_t modifier = keyboard_get_modifier();
    if (len)
    {
        vterm_mouse_event_t *e = RING_AT(&vterm_mouse_queue, len - 1);
        const bool full = len == VTERM_MOUSE_QUEUE;
        if (full || (!edge && !e->edge))
        {
            /* A merge that does not fit takes a new change, or with a full
             * queue holds the counters at their ends, so the direction is
             * kept. */
            bool fits = true;
            const int8_t wheel = vterm_mouse_add(e->wheel, dwheel, &fits);
            const int8_t pan = vterm_mouse_add(e->pan, dpan, &fits);
            if (fits || full)
            {
                e->x = x;
                e->y = y;
                e->flags = flags;
                e->modifier = modifier;
                e->wheel = wheel;
                e->pan = pan;
                e->edge |= edge;
                e->moved |= moved_by_user;
                e->shared = shared;
                return;
            }
        }
    }
    const vterm_mouse_event_t e = {
        .x = x,
        .y = y,
        .flags = flags,
        .modifier = modifier,
        .wheel = dwheel,
        .pan = dpan,
        .edge = edge,
        .moved = moved_by_user,
        .shared = shared,
    };
    RING_PUT(&vterm_mouse_queue, e);
}

/* Mode 0's cells: its rows down from begin, and the columns across from the
 * canvas's left edge, 8 pixels each. Nothing is over them while mode 0 is
 * not placed. */
static bool vterm_mouse_over(int x, int y)
{
    const vterm_placement_t *p = &vterm_placement;
    return p->placed && x >= 0 && x < p->cols * 8 &&
           y >= p->begin && y < p->begin + p->rows * p->row_px;
}

/* Whether the terminal takes a change that no held button has decided: the
 * contact is there, the program shared the tablet as the change came, and
 * the contact is over mode 0's cells. */
static bool vterm_mouse_takes(const vterm_mouse_event_t *e)
{
    return (e->flags & VTERM_MOUSE_PRESENT) && e->shared && vterm_mouse_over(e->x, e->y);
}

static bool vterm_mouse_shift(const vterm_mouse_event_t *e)
{
    return e->modifier & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT);
}

bool vterm_mouse_acts(void)
{
    return vterm_mouse_acting;
}

/* ---- the screen the pointer is over -------------------------------------- */

/* A place on the screen showing: a row of the screen, and a column of that
 * row's own, which may be one past its last. */
typedef struct
{
    int y, x;
} vterm_mouse_pos_t;

/* Which comes first on the screen: below 0 for a, above for b. */
static int vterm_mouse_cmp(vterm_mouse_pos_t a, vterm_mouse_pos_t b)
{
    return a.y != b.y ? a.y - b.y : a.x - b.x;
}

/* The grid showing, and its screen showing, which the pointer is over. */
typedef struct
{
    term_grid_t *g;
    screen_t *s;
    uint8_t code; // as vterm_mouse_gesture.screen
} vterm_mouse_view_t;

static vterm_mouse_view_t vterm_mouse_view(void)
{
    term_grid_t *g = vterm_get_shown(&vterm);
    const uint8_t screen = (uint8_t)(g->shown - g->screen);
    return (vterm_mouse_view_t){g, g->shown, (uint8_t)((g == &vterm_80.grid) << 1 | screen)};
}

/* The column of row y of the screen under canvas pixel x, mode 0 placed:
 * one past the right of the row is one past its last column. A row of
 * double width has half the columns, 16 pixels each. */
static int vterm_mouse_col(const vterm_mouse_view_t *v, int y, int x)
{
    const bool wide = screen_size(v->s, (uint8_t)y);
    const int cols = wide ? v->g->width / 2 : v->g->width;
    x = x < 0 ? 0 : x / (wide ? 16 : 8);
    return x > cols ? cols : x;
}

/* The cell under canvas pixel x, y, mode 0 placed: a pointer above or below
 * mode 0's rows is on its first or last row. */
static vterm_mouse_pos_t vterm_mouse_at(const vterm_mouse_view_t *v, int x, int y)
{
    const vterm_placement_t *p = &vterm_placement;
    vterm_mouse_pos_t at = {y < p->begin ? 0 : (y - p->begin) / p->row_px, 0};
    if (at.y >= v->g->height)
        at.y = v->g->height - 1;
    at.x = vterm_mouse_col(v, at.y, x);
    return at;
}

static vterm_mouse_cell_t vterm_mouse_cell(const vterm_mouse_view_t *v, vterm_mouse_pos_t p)
{
    return (vterm_mouse_cell_t){screen_mem(v->s, (uint8_t)p.y), (uint8_t)p.x, 0};
}

/* Where a point is on the screen now: its row of memory's row of the
 * screen, which the store keeps a row of the height, and its column, no
 * more than that row has now. */
static vterm_mouse_pos_t vterm_mouse_pos(const vterm_mouse_view_t *v, vterm_mouse_cell_t c)
{
    uint8_t y = 0;
    while (y < v->g->height - 1 && screen_mem(v->s, y) != c.row)
        y++;
    assert(screen_mem(v->s, y) == c.row);
    const int cols = screen_size(v->s, y) ? v->g->width / 2 : v->g->width;
    return (vterm_mouse_pos_t){y, c.col < cols ? c.col : cols};
}

/* Whether two points are on the same cell, or gap, wherever in it. */
static bool vterm_mouse_cell_eq(vterm_mouse_cell_t a, vterm_mouse_cell_t b)
{
    return a.row == b.row && a.col == b.col;
}

/* The pixels across a cell of row y of the screen: 16 on a row of double
 * width. */
static int vterm_mouse_cell_px(const vterm_mouse_view_t *v, int y)
{
    return screen_size(v->s, (uint8_t)y) ? 16 : 8;
}

/* The point of canvas pixel x, over the cell at, mode 0 placed: inside the
 * cell, at its place; past the row's columns, the edge past its last. */
static vterm_mouse_cell_t vterm_mouse_point(const vterm_mouse_view_t *v, vterm_mouse_pos_t at,
                                            int x)
{
    vterm_mouse_cell_t c = vterm_mouse_cell(v, at);
    const int w = vterm_mouse_cell_px(v, at.y);
    if (at.x < v->g->width * 8 / w && x >= at.x * w)
        c.sub = (uint8_t)(1 + (x - at.x * w) * SCREEN_MARK_SUBS / w);
    return c;
}

/* Where in its cell point c is now, as vterm_mouse_pos puts it at p: its
 * left edge for one past the row's columns now. */
static int vterm_mouse_sub(vterm_mouse_cell_t c, vterm_mouse_pos_t p)
{
    return p.x == c.col ? c.sub : 0;
}

/* Which point comes first on the screen, to the place in a cell: below 0
 * for a, above for b. */
static int vterm_mouse_point_cmp(const vterm_mouse_view_t *v, vterm_mouse_cell_t a,
                                 vterm_mouse_cell_t b)
{
    const vterm_mouse_pos_t pa = vterm_mouse_pos(v, a), pb = vterm_mouse_pos(v, b);
    const int d = vterm_mouse_cmp(pa, pb);
    return d ? d : vterm_mouse_sub(a, pa) - vterm_mouse_sub(b, pb);
}

/* The pixel of its cell's width point c is in, at p, from 0, or -1 for a
 * point at a cell's edge. */
static int vterm_mouse_px(const vterm_mouse_view_t *v, vterm_mouse_cell_t c, vterm_mouse_pos_t p)
{
    const int sub = vterm_mouse_sub(c, p);
    return sub ? (sub - 1) * vterm_mouse_cell_px(v, p.y) / SCREEN_MARK_SUBS : -1;
}

/* One row of the screen as the walks below read it: its text, and the
 * column of its last written cell, -1 for none. */
typedef struct
{
    const vterm_mouse_view_t *v;
    int y;
    int cols;
    int last;
    uint16_t text[VTERM_MOUSE_MAX_COLS];
} vterm_mouse_line_t;

static void vterm_mouse_load(vterm_mouse_line_t *l, int y)
{
    l->y = y;
    l->cols = (int)screen_row_text(l->v->s, (uint8_t)y, l->text);
    l->last = l->cols - 1;
    while (l->last >= 0 && !(l->text[l->last] & CELL_TEXT_WRITTEN))
        l->last--;
}

/* xterm's default character classes: a word is a run of one class. A range
 * of IDENT is a class of its own for each character, so each of those is a
 * word by itself. Later ranges win over earlier ones, as xterm sets them. */
enum
{
    VTERM_MOUSE_IDENT = -1,
    VTERM_MOUSE_CNTRL = 1,
    VTERM_MOUSE_BLANK = 32,
    VTERM_MOUSE_ALNUM = 48,
};

static const struct
{
    uint16_t first, last;
    int16_t cclass;
} vterm_mouse_classes[] = {
    {0x0000, 0x0000, VTERM_MOUSE_BLANK},
    {0x0001, 0x001F, VTERM_MOUSE_CNTRL},
    {0x0009, 0x0009, VTERM_MOUSE_BLANK},
    {0x0030, 0x0039, VTERM_MOUSE_ALNUM},
    {0x0041, 0x005A, VTERM_MOUSE_ALNUM},
    {0x005F, 0x005F, VTERM_MOUSE_ALNUM},
    {0x0061, 0x007A, VTERM_MOUSE_ALNUM},
    {0x007F, 0x009F, VTERM_MOUSE_CNTRL},
    {0x00A0, 0x00BF, VTERM_MOUSE_IDENT},
    {0x00C0, 0x00FF, VTERM_MOUSE_ALNUM},
    {0x00D7, 0x00D7, VTERM_MOUSE_IDENT},
    {0x00F7, 0x00F7, VTERM_MOUSE_IDENT},
    {0x0100, 0xFFDF, VTERM_MOUSE_ALNUM},
    {0x037E, 0x037E, VTERM_MOUSE_IDENT},
    {0x0387, 0x0387, VTERM_MOUSE_IDENT},
    {0x055A, 0x055F, VTERM_MOUSE_IDENT},
    {0x0589, 0x0589, VTERM_MOUSE_IDENT},
    {0x0700, 0x070D, VTERM_MOUSE_IDENT},
    {0x104A, 0x104F, VTERM_MOUSE_IDENT},
    {0x10FB, 0x10FB, VTERM_MOUSE_IDENT},
    {0x1361, 0x1368, VTERM_MOUSE_IDENT},
    {0x166D, 0x166E, VTERM_MOUSE_IDENT},
    {0x17D4, 0x17DC, VTERM_MOUSE_IDENT},
    {0x1800, 0x180A, VTERM_MOUSE_IDENT},
    {0x2000, 0x200A, VTERM_MOUSE_BLANK},
    {0x200B, 0x200F, VTERM_MOUSE_CNTRL},
    {0x2010, 0x27FF, VTERM_MOUSE_IDENT},
    {0x202A, 0x202E, VTERM_MOUSE_CNTRL},
    {0x2060, 0x206F, VTERM_MOUSE_CNTRL},
    {0x2070, 0x207F, 0x2070},
    {0x2080, 0x208F, 0x2080},
    {0x3000, 0x3000, VTERM_MOUSE_BLANK},
    {0x3001, 0x3020, VTERM_MOUSE_IDENT},
    {0x3040, 0x309F, 0x3040},
    {0x30A0, 0x30FF, 0x30A0},
    {0x3300, 0x9FFF, 0x4E00},
    {0xAC00, 0xD7A3, 0xAC00},
    {0xF900, 0xFAFF, 0x4E00},
    {0xFE30, 0xFE6B, VTERM_MOUSE_IDENT},
    {0xFEFF, 0xFEFF, VTERM_MOUSE_CNTRL},
    {0xFF00, 0xFF0F, VTERM_MOUSE_IDENT},
    {0xFF1A, 0xFF20, VTERM_MOUSE_IDENT},
    {0xFF3B, 0xFF40, VTERM_MOUSE_IDENT},
    {0xFF5B, 0xFF64, VTERM_MOUSE_IDENT},
    {0xFFF9, 0xFFFB, VTERM_MOUSE_CNTRL},
};

static int32_t vterm_mouse_class_of(uint32_t u)
{
    int32_t c = VTERM_MOUSE_IDENT;
    for (size_t i = 0; i < sizeof vterm_mouse_classes / sizeof *vterm_mouse_classes; i++)
        if (u >= vterm_mouse_classes[i].first && u <= vterm_mouse_classes[i].last)
            c = vterm_mouse_classes[i].cclass;
    return c == VTERM_MOUSE_IDENT ? (int32_t)u : c;
}

/* The class of column x of the row: the glyph's, a cell never written a
 * blank's, and none past the row's columns. */
static int32_t vterm_mouse_class(const vterm_mouse_line_t *l, int x)
{
    if (x < 0 || x >= l->cols)
        return 0;
    const uint16_t word = l->text[x];
    return word & CELL_TEXT_WRITTEN ? vterm_mouse_class_of(cell_text_unicode(word))
                                    : VTERM_MOUSE_BLANK;
}

static bool vterm_mouse_wrapped(const vterm_mouse_view_t *v, int y)
{
    return screen_wrapped(v->s, (uint8_t)y);
}

/* Row y follows one that wrapped onto it. */
static bool vterm_mouse_continued(const vterm_mouse_view_t *v, int y)
{
    return y > 0 && vterm_mouse_wrapped(v, y - 1);
}

static bool vterm_mouse_more(const vterm_mouse_view_t *v, int y)
{
    return y < v->g->height - 1;
}

/* xterm's okPosition, for words: a point past a row's text, but on its
 * last row, is the start of the next row. False when it moved. */
static bool vterm_mouse_settle(vterm_mouse_line_t *l, vterm_mouse_pos_t *p)
{
    if (p->x <= l->last + 1 || !vterm_mouse_more(l->v, p->y))
        return true;
    p->x = 0;
    vterm_mouse_load(l, ++p->y);
    return false;
}

/* What is selected between two points, as the unit widens them, as xterm's
 * ComputeSelect: by characters, just that, but for a highlight a program
 * tracks, as by words; by words, out to the ends of the
 * words under them, across a row that wrapped, a point past a row's text,
 * but on its last row, first going to the start of the next row
 * (vterm_mouse_settle); by lines, from the first row of the line under the
 * first to the start of the row after the line under the last, or to the
 * end of its text on the screen's last row. */
static void vterm_mouse_widen(const vterm_mouse_view_t *v, uint8_t unit,
                              vterm_mouse_pos_t *start, vterm_mouse_pos_t *end)
{
    vterm_mouse_line_t ls = {.v = v}, le = {.v = v};
    vterm_mouse_load(&ls, start->y);
    vterm_mouse_load(&le, end->y);
    switch (unit)
    {
    case VTERM_MOUSE_CHAR:
        /* A highlight a program tracks keeps xterm's rule, so its reports
         * name what xterm's would. */
        if (vterm_mouse_tracking == VTERM_MOUSE_TRACK_ON)
        {
            vterm_mouse_settle(&ls, start);
            vterm_mouse_settle(&le, end);
        }
        break;
    case VTERM_MOUSE_WORD:
        if (vterm_mouse_settle(&ls, start))
        {
            const int32_t cclass = vterm_mouse_class(&ls, start->x);
            do
            {
                if (--start->x < 0 && vterm_mouse_continued(v, start->y))
                {
                    vterm_mouse_load(&ls, --start->y);
                    start->x = ls.last;
                }
            } while (start->x >= 0 && vterm_mouse_class(&ls, start->x) == cclass);
            start->x++;
        }
        if (vterm_mouse_settle(&le, end))
        {
            const int32_t cclass = vterm_mouse_class(&le, end->x);
            do
            {
                if (++end->x > le.last && vterm_mouse_wrapped(v, end->y))
                {
                    if (!vterm_mouse_more(v, end->y))
                        break;
                    end->x = 0;
                    vterm_mouse_load(&le, ++end->y);
                }
            } while (end->x <= le.last && vterm_mouse_class(&le, end->x) == cclass);
            if (end->x > le.last + 1 && vterm_mouse_more(v, end->y))
            {
                end->x = 0;
                end->y++;
            }
        }
        break;
    case VTERM_MOUSE_LINE:
        while (vterm_mouse_wrapped(v, end->y) && vterm_mouse_more(v, end->y))
            vterm_mouse_load(&le, ++end->y);
        start->x = 0;
        while (vterm_mouse_continued(v, start->y))
            start->y--;
        if (vterm_mouse_more(v, end->y))
        {
            end->x = 0;
            end->y++;
        }
        else
            end->x = le.last + 1;
        break;
    }
}

/* What the gesture selects on the screen: what is between its points, in
 * order, widened by its unit. It is the selection's ends when the gesture
 * made the selection, and holds when output has dropped its highlight. */
static void vterm_mouse_span(const vterm_mouse_view_t *v, vterm_mouse_pos_t *start,
                             vterm_mouse_pos_t *end)
{
    *start = vterm_mouse_pos(v, vterm_mouse_gesture.ext_start);
    *end = vterm_mouse_pos(v, vterm_mouse_gesture.ext_end);
    if (vterm_mouse_cmp(*start, *end) > 0)
    {
        const vterm_mouse_pos_t t = *start;
        *start = *end;
        *end = t;
    }
    vterm_mouse_widen(v, vterm_mouse_gesture.unit, start, end);
}

/* Whether the selection is what the gesture selects, a and b: showing, or
 * empty. */
static bool vterm_mouse_is(const vterm_mouse_view_t *v, vterm_mouse_cell_t a,
                           vterm_mouse_cell_t b)
{
    const screen_selection_t *sel = &vterm_selection;
    return sel->screen == v->s && sel->start_row == a.row && sel->start_col == a.col &&
           sel->end_row == b.row && sel->end_col == b.col && !sel->left &&
           sel->right == SCREEN_SELECTION_EDGE && (sel->shows || vterm_mouse_cell_eq(a, b));
}

/* The selection becomes what the gesture selects, the primary's, which
 * ends the line editor's: the terminal has one selection, which the two
 * share. One that is already so, showing or empty, is left as it is, so
 * the RIA sends only a change. */
static void vterm_mouse_compute(const vterm_mouse_view_t *v)
{
    vterm_mouse_pos_t start, end;
    vterm_mouse_span(v, &start, &end);
    const vterm_mouse_cell_t a = vterm_mouse_cell(v, start), b = vterm_mouse_cell(v, end);
    if (vterm_mouse_is(v, a, b))
        return;
    rln_unselect();
    vterm_select(v->g, v->code & 1, a.row, a.col, b.row, b.col, 0, SCREEN_SELECTION_EDGE);
}

/* Whether the selection showing on the screen showing is the gesture's,
 * the primary's, and not the line editor's, which the last press made
 * when it was in the line. */
static bool vterm_mouse_shows(const vterm_mouse_view_t *v)
{
    if (!vterm_selection.shows || !vterm_mouse_gesture.clicks || vterm_mouse_gesture.line ||
        vterm_mouse_gesture.screen != v->code)
        return false;
    vterm_mouse_pos_t start, end;
    vterm_mouse_span(v, &start, &end);
    return vterm_mouse_is(v, vterm_mouse_cell(v, start), vterm_mouse_cell(v, end));
}

/* Whether a press of the left button at the cell is another click of the
 * last, by the selection's rule: that one counted, on the same cell of the
 * same screen, and this comes within VTERM_MOUSE_CLICK_MS of its release. */
static bool vterm_mouse_again(const vterm_mouse_view_t *v, vterm_mouse_pos_t at)
{
    return vterm_mouse_gesture.clicked && vterm_mouse_gesture.screen == v->code &&
           vterm_mouse_cell_eq(vterm_mouse_cell(v, at), vterm_mouse_gesture.click) &&
           !timer_mach_passed(vterm_mouse_gesture.click_by);
}

/* A press at point p, held by button: the next click of the run when it
 * goes on (again), else the first, and the unit that makes; counted, it
 * counts for the next by the selection's rule. A press on another screen
 * than the gesture's puts all the gesture's points at the point, so the
 * store keeps none on a screen they are not of. */
static void vterm_mouse_press(const vterm_mouse_view_t *v, vterm_mouse_cell_t p, uint8_t button,
                              bool counted, bool again)
{
    vterm_mouse_gesture.clicks =
        (uint8_t)(again ? vterm_mouse_gesture.clicks % VTERM_MOUSE_CLICKS + 1 : 1);
    if (vterm_mouse_gesture.screen != v->code)
        for (unsigned i = 0; i < VTERM_MOUSE_POINTS; i++)
            vterm_mouse_gesture.points[i] = p;
    vterm_mouse_gesture.unit = vterm_mouse_units[vterm_mouse_gesture.clicks - 1];
    vterm_mouse_gesture.clicked = counted;
    vterm_mouse_gesture.click = p;
    vterm_mouse_gesture.screen = v->code;
    vterm_mouse_mark();
    vterm_mouse_gesture.button = button;
    vterm_mouse_gesture.line = false;
    vterm_mouse_gesture.dragged = false;
}

/* By characters, the end the drag moves goes to the gap nearest point at,
 * from the press's character, or the gap an end that extends holds at:
 * xterm's ExtendExtend, whose drag that crosses the end it holds turns
 * round to hold the other end as the drag began, held as the rule of the
 * gesture says (the comment at its start). The point the drag holds from
 * is save_start going on and save_end going back: both the press, or the
 * two ends of a selection extended. */
static void vterm_mouse_chars(const vterm_mouse_view_t *v, vterm_mouse_cell_t at)
{
    vterm_mouse_gesture_t *gs = &vterm_mouse_gesture;
    if (gs->left && vterm_mouse_point_cmp(v, at, gs->save_end) > 0)
        gs->left = false;
    else if (!gs->left && vterm_mouse_point_cmp(v, at, gs->save_start) < 0)
        gs->left = true;
    const bool back = gs->left;
    const vterm_mouse_cell_t from = back ? gs->save_end : gs->save_start;
    const vterm_mouse_pos_t f = vterm_mouse_pos(v, from);
    const int px = vterm_mouse_px(v, from, f);
    const int d = vterm_mouse_point_cmp(v, at, from);
    /* The gap held, and the gap the end the drag moves goes no nearer to it
     * than. From a gap, as an end that extends holds at, both that gap.
     * From a press inside a character, the character is taken whole: held
     * from its left edge going on, or its right edge going back, and the
     * end out to its other edge. But from a press in its last pixel column
     * with the pointer past it going on, or its first with the pointer
     * before it going back, it is not taken: the gap held is its edge on
     * the pointer's side. */
    vterm_mouse_pos_t held = f, edge = f;
    if (px >= 0)
    {
        const bool margin =
            back ? px == 0 && d < 0 : px == vterm_mouse_cell_px(v, f.y) - 1 && d > 0;
        held.x += back != margin;
        edge.x += !back;
    }
    /* Nothing is selected until the pointer is away from the point held
     * from: for a press, once the drag has moved, by any pixel, any way. */
    vterm_mouse_pos_t to = held;
    if (px >= 0 ? gs->dragged : d != 0)
    {
        /* The gap nearest the pointer, a character joining once the
         * pointer passes its middle, but never nearer than edge. */
        to = vterm_mouse_pos(v, at);
        if (vterm_mouse_sub(at, to) > SCREEN_MARK_SUBS / 2)
            to.x++;
        if (back ? vterm_mouse_cmp(to, edge) > 0 : vterm_mouse_cmp(to, edge) < 0)
            to = edge;
    }
    gs->ext_start = vterm_mouse_cell(v, back ? to : held);
    gs->ext_end = vterm_mouse_cell(v, back ? held : to);
}

/* The end the drag moves goes to point at: by characters by
 * vterm_mouse_chars's rule, and by words and lines to its cell. */
static void vterm_mouse_move(const vterm_mouse_view_t *v, vterm_mouse_cell_t at)
{
    if (vterm_mouse_gesture.unit == VTERM_MOUSE_CHAR)
    {
        vterm_mouse_chars(v, at);
        return;
    }
    at.sub = 0;
    if (vterm_mouse_gesture.left)
        vterm_mouse_gesture.ext_start = at;
    else
        vterm_mouse_gesture.ext_end = at;
}

/* The gesture holds from point raw: where the run's first click started,
 * and the press the drag holds from, with nothing selected yet. */
static void vterm_mouse_hold(const vterm_mouse_view_t *v, vterm_mouse_cell_t raw)
{
    vterm_mouse_gesture_t *gs = &vterm_mouse_gesture;
    gs->raw = gs->save_start = gs->save_end = raw;
    gs->ext_start = gs->ext_end = vterm_mouse_cell(v, vterm_mouse_pos(v, raw));
    gs->left = false;
}

/* A selection starts, as xterm's StartSelect: from point raw, where the
 * run's first click started, which a first click sets, to point at, in the
 * unit the clicks made, and the line editor's ends: one selection at a
 * time. */
static void vterm_mouse_begin(const vterm_mouse_view_t *v, vterm_mouse_cell_t raw,
                              vterm_mouse_cell_t at)
{
    rln_unselect();
    vterm_mouse_hold(v, raw);
    vterm_mouse_gesture.left = vterm_mouse_gesture.unit != VTERM_MOUSE_CHAR &&
                               vterm_mouse_cmp(vterm_mouse_pos(v, at), vterm_mouse_pos(v, raw)) < 0;
    vterm_mouse_move(v, at);
    vterm_mouse_compute(v);
}

/* A press at point p starts a selection there, by the unit its clicks make:
 * a counted one by the selection's rule. */
static void vterm_mouse_start(const vterm_mouse_view_t *v, vterm_mouse_cell_t p, uint8_t button,
                              bool counted)
{
    vterm_mouse_press(v, p, button, counted,
                      counted && vterm_mouse_again(v, vterm_mouse_pos(v, p)));
    vterm_mouse_begin(v, p, p);
}

/* What the drag selects goes to the line editor, for a drag in its line:
 * the end it holds and the end it moves, each a gap by characters and a
 * cell by words and lines, which the line takes as it lays them out
 * (rln_mouse_drag), once the pointer has moved from the press: until then
 * the press is a click there (rln_mouse_press). Otherwise it is the
 * selection. */
static void vterm_mouse_show(const vterm_mouse_view_t *v)
{
    const vterm_mouse_gesture_t *gs = &vterm_mouse_gesture;
    if (!gs->line)
    {
        vterm_mouse_compute(v);
        return;
    }
    if (!gs->dragged)
        return;
    const vterm_mouse_pos_t start = vterm_mouse_pos(v, gs->ext_start);
    const vterm_mouse_pos_t end = vterm_mouse_pos(v, gs->ext_end);
    const vterm_mouse_pos_t held = gs->left ? end : start, to = gs->left ? start : end;
    rln_mouse_drag((uint8_t)held.y, (uint8_t)held.x, (uint8_t)to.y, (uint8_t)to.x);
}

/* A press of the right button, or Shift with the left while no mouse mode
 * is set, takes the selection's nearer end to point p, at the cell at, as
 * xterm's start-extend, in the unit it was made in. With no selection of
 * the primary's showing there, it selects from the point as the left
 * button does. */
static void vterm_mouse_extend(const vterm_mouse_view_t *v, vterm_mouse_pos_t at,
                               vterm_mouse_cell_t p, uint8_t button)
{
    if (!vterm_mouse_shows(v))
    {
        vterm_mouse_start(v, p, button, false);
        return;
    }
    vterm_mouse_gesture.clicks = 1;
    vterm_mouse_gesture.clicked = false;
    vterm_mouse_gesture.button = button;
    vterm_mouse_pos_t lo = vterm_mouse_pos(v, vterm_mouse_gesture.ext_start);
    vterm_mouse_pos_t hi = vterm_mouse_pos(v, vterm_mouse_gesture.ext_end);
    if (vterm_mouse_cmp(lo, hi) > 0)
    {
        const vterm_mouse_pos_t t = lo;
        lo = hi;
        hi = t;
    }
    vterm_mouse_gesture.save_start = vterm_mouse_gesture.ext_start = vterm_mouse_cell(v, lo);
    vterm_mouse_gesture.save_end = vterm_mouse_gesture.ext_end = vterm_mouse_cell(v, hi);
    vterm_mouse_pos_t start, end;
    vterm_mouse_span(v, &start, &end);
    const int across = v->g->width + 1;
    const int here = at.y * across + at.x;
    const int to_start = abs(here - (start.y * across + start.x));
    const int to_end = abs(here - (end.y * across + end.x));
    vterm_mouse_gesture.left = to_start < to_end || vterm_mouse_cmp(at, start) < 0;
    vterm_mouse_move(v, p);
    vterm_mouse_compute(v);
}

/* A drag takes the end it moves to point at, as xterm's ExtendExtend: one
 * that crosses the other end turns round, and the other end goes back to
 * where it was as the drag began. By characters, vterm_mouse_chars
 * judges the crossing to the place in a cell. */
static void vterm_mouse_drag(const vterm_mouse_view_t *v, vterm_mouse_cell_t at)
{
    if (vterm_mouse_gesture.unit != VTERM_MOUSE_CHAR)
    {
        const vterm_mouse_pos_t p = vterm_mouse_pos(v, at);
        vterm_mouse_pos_t start, end;
        vterm_mouse_span(v, &start, &end);
        if (vterm_mouse_gesture.left && vterm_mouse_cmp(p, end) >= 0)
        {
            vterm_mouse_gesture.left = false;
            vterm_mouse_gesture.ext_start = vterm_mouse_gesture.save_start;
        }
        else if (!vterm_mouse_gesture.left && vterm_mouse_cmp(p, start) < 0)
        {
            vterm_mouse_gesture.left = true;
            vterm_mouse_gesture.ext_end = vterm_mouse_gesture.save_end;
        }
    }
    vterm_mouse_move(v, at);
    vterm_mouse_show(v);
}

/* Buffer which (clip.h) takes what is selected, if anything: the primary
 * as a drag ends. A selection that shows is the gesture's, on rows of the
 * height. */
static void vterm_mouse_keep(const vterm_mouse_view_t *v, uint8_t which)
{
    if (!vterm_selection.shows || vterm_selection.screen != v->s)
        return;
    const screen_selection_t *sel = &vterm_selection;
    const vterm_mouse_pos_t start =
        vterm_mouse_pos(v, (vterm_mouse_cell_t){sel->start_row, sel->start_col, 0});
    const vterm_mouse_pos_t end =
        vterm_mouse_pos(v, (vterm_mouse_cell_t){sel->end_row, sel->end_col, 0});
    clip_copy(which, v->s, (uint8_t)start.y, (uint8_t)start.x, (uint8_t)end.y, (uint8_t)end.x);
}

bool vterm_mouse_copy(void)
{
    const vterm_mouse_view_t v = vterm_mouse_view();
    if (!vterm_mouse_shows(&v))
        return false;
    vterm_mouse_keep(&v, CLIP_CLIPBOARD);
    return true;
}

/* A drag ends, as xterm's EndExtend notes it: the next click goes on from
 * it within VTERM_MOUSE_CLICK_MS, and by highlight tracking's rule only
 * when left, the drag of the left button selecting or highlighting on the
 * screen, not in the line editor's line. */
static void vterm_mouse_ended(bool left)
{
    vterm_mouse_gesture.click_by = timer_mach_in_ms(VTERM_MOUSE_CLICK_MS);
    vterm_mouse_gesture.ended_left = left;
}

/* The drag goes no further. */
static void vterm_mouse_let_go(void)
{
    vterm_mouse_gesture.button = 0;
}

/* A press of the left button on the line the line editor is editing, at
 * point p and index of the line, selects in the line, judged against the
 * line as it is laid out now, and ends the primary's highlight. The
 * gesture holds from the press, as a drag on the screen does. The gap
 * nearer the pointer is the one xterm's click puts the cursor at, column
 * (x + (W - 1) / 2) / W on cells W pixels wide: after the character from
 * pixel 5 of 8, and pixel 9 of 16. */
static void vterm_mouse_line(const vterm_mouse_view_t *v, vterm_mouse_cell_t p, uint8_t index)
{
    const vterm_mouse_pos_t at = vterm_mouse_pos(v, p);
    const int w = vterm_mouse_cell_px(v, at.y);
    const bool after = vterm_mouse_px(v, p, at) >= w - (w - 1) / 2;
    vterm_mouse_press(v, p, TABLET_FLAG_LEFT, true, vterm_mouse_again(v, at));
    vterm_mouse_hold(v, p);
    vterm_mouse_gesture.line = true;
    if (vterm_selection.shows)
        vterm_unselect();
    rln_mouse_press(index, after, vterm_mouse_gesture.clicks);
}

/* The most entries of the keyboard's queue (typed.h) one change the
 * terminal takes can need while a line is read: a drag in the line
 * editor's line, two, its release, and the middle button's paste. */
#define VTERM_MOUSE_ENTRIES 4

/* A change the terminal took, with the buttons that were down before it.
 * While a line is read, the task has seen to the queue's room. */
static void vterm_mouse_act(const vterm_mouse_event_t *e, uint8_t was)
{
    const uint8_t down = e->flags & ~was & VTERM_MOUSE_BUTTONS;
    const uint8_t up = was & ~e->flags & VTERM_MOUSE_BUTTONS;
    if (!vterm_placement.placed)
    {
        vterm_mouse_let_go();
        return;
    }
    const vterm_mouse_view_t v = vterm_mouse_view();
    const vterm_mouse_pos_t at = vterm_mouse_at(&v, e->x, e->y);
    const vterm_mouse_cell_t p = vterm_mouse_point(&v, at, e->x);
    /* A drag on a screen no longer showing goes no further. */
    if (vterm_mouse_gesture.button && vterm_mouse_gesture.screen != v.code)
        vterm_mouse_let_go();
    /* A contact that has gone, as a finger lifted, is nowhere, so its
     * release ends the drag where it last was. A drag in the line editor's
     * line stays in the line, and the line fills the primary with what it
     * selected, once it takes the release (rln_mouse_release). */
    if (vterm_mouse_gesture.button && (e->flags & VTERM_MOUSE_PRESENT) &&
        (e->moved || (up & vterm_mouse_gesture.button)))
    {
        vterm_mouse_gesture.dragged |= e->moved;
        vterm_mouse_drag(&v, p);
    }
    if (up & vterm_mouse_gesture.button)
    {
        if (vterm_mouse_gesture.line)
            rln_mouse_release();
        else
            vterm_mouse_keep(&v, CLIP_PRIMARY);
        vterm_mouse_ended(vterm_mouse_gesture.button == TABLET_FLAG_LEFT &&
                          !vterm_mouse_gesture.line);
        vterm_mouse_let_go();
    }
    /* A left click counts on from the last only when no other button went
     * down or came up between them, and a press that extends counts none
     * (vterm_mouse_extend). */
    if ((down | up) & ~TABLET_FLAG_LEFT)
        vterm_mouse_gesture.clicked = false;
    if (up & TABLET_FLAG_MIDDLE)
        clip_paste(CLIP_PRIMARY);
    if (vterm_mouse_gesture.button)
        return;
    /* While a program has asked for the mouse's events, what reaches the
     * terminal comes with Shift (vterm_mouse_asked): Shift with the left
     * button then selects, as the left button alone does with none asked,
     * and never in the line editor's line. */
    const bool shift = vterm_mouse_shift(e);
    const bool extends = shift && vterm.mouse_event == TERM_MOUSE_OFF;
    /* A selection that starts or extends frees the rows a highlight that
     * goes on from it is held to, as xterm's select-start and start-extend
     * set its valid rows to all. */
    if (down & (TABLET_FLAG_LEFT | TABLET_FLAG_RIGHT))
    {
        vterm_mouse_track_first = 0;
        vterm_mouse_track_last = VTERM_MOUSE_TRACK_ALL;
    }
    const int index = (down & TABLET_FLAG_LEFT) && !shift ? rln_index_at((uint8_t)at.y, (uint8_t)at.x)
                                                           : -1;
    if (index >= 0)
        vterm_mouse_line(&v, p, (uint8_t)index);
    else if ((down & TABLET_FLAG_LEFT) && !extends)
        vterm_mouse_start(&v, p, TABLET_FLAG_LEFT, true);
    else if (down & TABLET_FLAG_LEFT)
        vterm_mouse_extend(&v, at, p, TABLET_FLAG_LEFT);
    else if (down & TABLET_FLAG_RIGHT)
        vterm_mouse_extend(&v, at, p, TABLET_FLAG_RIGHT);
}

/* ---- reports ------------------------------------------------------------
 *
 * The reports a program asks for with a mouse mode (term.h's mouse_event),
 * as xterm's, in the form the terminal is in (vtmouse.h): X10's events tell
 * a press of the left, middle or right button; the normal events, and
 * highlight tracking's, every press and release and the wheel a notch at a
 * time; the button events those and motion with a button held; and the any
 * events every motion too. Motion is told only to a new cell, or pixel in
 * the pixel form, after the last report. A change with Shift held is told
 * nothing: it is the terminal's, as xterm's. The positions are mode 0's,
 * a pointer outside its rows on the nearest, as a drag that leaves them
 * goes on to tell. The reports are typed (typed.h) in order with the keys,
 * each whole: one with no room waits there, with what is left of its
 * change, for the keys before it to be read. */

/* The buttons that make a report, in the order motion names the one held:
 * each one's TABLET_FLAG_, its VTMOUSE_, and its DEC locator number, or
 * none for one the locator has not. */
#define VTERM_MOUSE_NO_LOCATOR 0xFF
static const struct
{
    uint8_t flag, button, locator;
} vterm_mouse_buttons[] = {
    {TABLET_FLAG_LEFT, VTMOUSE_LEFT, VTMOUSE_LOCATOR_LEFT},
    {TABLET_FLAG_MIDDLE, VTMOUSE_MIDDLE, VTMOUSE_LOCATOR_MIDDLE},
    {TABLET_FLAG_RIGHT, VTMOUSE_RIGHT, VTMOUSE_LOCATOR_RIGHT},
    {TABLET_FLAG_BTN4, VTMOUSE_BTN4, VTMOUSE_LOCATOR_M4},
    {TABLET_FLAG_BTN5, VTMOUSE_BTN5, VTERM_MOUSE_NO_LOCATOR},
};
#define VTERM_MOUSE_BUTTON_COUNT (sizeof vterm_mouse_buttons / sizeof *vterm_mouse_buttons)

/* Whether the program that asked for the mouse's events takes a change
 * the terminal takes: one with Shift held stays the terminal's. */
static bool vterm_mouse_asked(const vterm_mouse_event_t *e)
{
    return vterm.mouse_event != TERM_MOUSE_OFF && !vterm_mouse_shift(e);
}

/* Where a report puts canvas pixel x, y: mode 0's cell there, and its
 * pixel from mode 0's first, held to mode 0's rows and columns. */
typedef struct
{
    uint16_t col, row, x, y;
} vterm_mouse_spot_t;

static vterm_mouse_spot_t vterm_mouse_spot(int x, int y)
{
    const vterm_placement_t *p = &vterm_placement;
    const int width = p->cols * 8;
    const int height = p->rows * p->row_px;
    y -= p->begin;
    x = x < 0 ? 0 : x < width ? x : width - 1;
    y = y < 0 ? 0 : y < height ? y : height - 1;
    return (vterm_mouse_spot_t){(uint16_t)(x / 8), (uint16_t)(y / p->row_px), (uint16_t)x,
                                (uint16_t)y};
}

/* Bytes typed whole: false, with nothing typed, when the queue has no room
 * for them. */
static bool vterm_mouse_type(const char *out, size_t n)
{
    if (n > typed_free())
        return false;
    typed_push(out, n);
    return true;
}

/* One report, typed whole: false, with nothing typed, when the queue has no
 * room for it. X10's events carry no modifiers. A button the events do not
 * tell makes none, and leaves where the last report put the pointer. */
static bool vterm_mouse_send(uint8_t event, uint8_t button, uint8_t modifier,
                             const vterm_mouse_spot_t *at)
{
    char out[VTMOUSE_MAX];
    const uint8_t form = vterm.mouse_form;
    const uint8_t mods = event == VTMOUSE_X10 ? 0 : vtkey_modifiers(modifier);
    const uint8_t n = vtmouse_encode(out, form, event, button, mods, at->col, at->row, at->x, at->y);
    if (!vterm_mouse_type(out, n))
        return false;
    if (!n)
        return true;
    const bool pixels = form == TERM_MOUSE_FORM_PIXELS;
    vterm_mouse_sent_x = pixels ? at->x : at->col;
    vterm_mouse_sent_y = pixels ? at->y : at->row;
    return true;
}

/* ---- highlight tracking -------------------------------------------------
 *
 * As xterm's: the press of the left button is told as the normal events
 * tell it, and waits for the program's answer until it comes up. An answer
 * that starts the highlight makes it a drag of the gesture's, by
 * characters, from where the program said, held to the rows it said, and
 * highlighted as a selection is; any other answer, or none, leaves the
 * press a normal event, whose release is told. A press of the left button
 * that goes on from the last drag to end, as xterm's TrackDown counts it,
 * is told nothing and highlights at once, in the unit its place in the run
 * makes, from where the run's first click started, held to the rows the
 * last highlight was. The release of a highlight tells what it
 * highlighted, which then goes, and the primary takes it, as xterm's
 * EndExtend ends in SelectSet. */

/* The cell of the screen under canvas pixel x, y as the drag takes it, held
 * to the rows the highlight is held to, and to the screen's rows, which may
 * have become fewer since. */
static vterm_mouse_pos_t vterm_mouse_track_at(const vterm_mouse_view_t *v, int x, int y)
{
    vterm_mouse_pos_t at = vterm_mouse_at(v, x, y);
    const int most = v->g->height - 1;
    const int first = vterm_mouse_track_first < most ? vterm_mouse_track_first : most;
    const int last = vterm_mouse_track_last < most ? vterm_mouse_track_last : most;
    if (at.y < first)
        at.y = first;
    else if (at.y > last)
        at.y = last;
    at.x = vterm_mouse_col(v, at.y, x);
    return at;
}

/* A row of the screen showing from a row the program gave, held to it. */
static uint8_t vterm_mouse_track_row(const vterm_mouse_view_t *v, int row)
{
    return (uint8_t)(row < 0 ? 0 : row >= v->g->height ? v->g->height - 1 : row);
}

void vterm_mouse_track(int func, int col, int row, int first, int last)
{
    if (vterm_mouse_tracking != VTERM_MOUSE_TRACK_WAIT)
        return;
    vterm_mouse_tracking = VTERM_MOUSE_TRACK_OFF;
    if (!func || !vterm_placement.placed || vterm.mouse_event != TERM_MOUSE_HILITE)
        return;
    const vterm_mouse_view_t v = vterm_mouse_view();
    vterm_mouse_track_first = vterm_mouse_track_row(&v, first);
    vterm_mouse_track_last = vterm_mouse_track_row(&v, last);
    vterm_mouse_pos_t at = {vterm_mouse_track_row(&v, row), 0};
    const int cols = screen_size(v.s, (uint8_t)at.y) ? v.g->width / 2 : v.g->width;
    at.x = col < 0 ? 0 : col > cols ? cols : col;
    /* The press counted its click on the screen showing then: on another
     * now, the run starts again. The highlight's ends are cells, as
     * xterm's reports name them, so it holds at the gap before the cell
     * the program said. */
    const vterm_mouse_cell_t c = vterm_mouse_cell(&v, at);
    if (vterm_mouse_gesture.screen != v.code)
        vterm_mouse_press(&v, c, TABLET_FLAG_LEFT, false, false);
    vterm_mouse_gesture.button = TABLET_FLAG_LEFT;
    vterm_mouse_begin(&v, c, c);
    vterm_mouse_tracking = VTERM_MOUSE_TRACK_ON;
}

/* A press of the left button the program is to be told of, as xterm's
 * TrackDown: one that goes on from the last drag to end, which the left
 * button ended within VTERM_MOUSE_CLICK_MS, wherever it is, highlights at
 * once, in the unit its place in the run makes, from where the run's first
 * click started (xterm's rawPos), held to the rows the last highlight was
 * (xterm's valid rows), and is told nothing. The run is on the gesture's
 * screen, so a press on another starts it again. False for any other
 * press, a first click, which is then told and waits for the answer
 * (vterm_mouse_track_wait). */
static bool vterm_mouse_track_down(const vterm_mouse_event_t *e)
{
    const vterm_mouse_view_t v = vterm_mouse_view();
    if (!vterm_mouse_gesture.ended_left || vterm_mouse_gesture.screen != v.code ||
        timer_mach_passed(vterm_mouse_gesture.click_by))
        return false;
    const vterm_mouse_cell_t at = vterm_mouse_cell(&v, vterm_mouse_track_at(&v, e->x, e->y));
    const vterm_mouse_cell_t raw =
        vterm_mouse_cell(&v, vterm_mouse_pos(&v, vterm_mouse_gesture.raw));
    vterm_mouse_press(&v, at, TABLET_FLAG_LEFT, true, true);
    vterm_mouse_begin(&v, raw, at);
    vterm_mouse_tracking = VTERM_MOUSE_TRACK_ON;
    return true;
}

/* A press of the left button told: it waits for the program's answer, and
 * is a first click, held by no button until an answer starts the
 * highlight. */
static void vterm_mouse_track_wait(const vterm_mouse_event_t *e)
{
    const vterm_mouse_view_t v = vterm_mouse_view();
    vterm_mouse_press(&v, vterm_mouse_cell(&v, vterm_mouse_at(&v, e->x, e->y)), 0, true, false);
    vterm_mouse_tracking = VTERM_MOUSE_TRACK_WAIT;
}

/* The highlight goes, and the drag with it. */
static void vterm_mouse_track_off(void)
{
    vterm_mouse_tracking = VTERM_MOUSE_TRACK_OFF;
    vterm_mouse_let_go();
    if (vterm_selection.shows)
        vterm_unselect();
}

/* A change while the drag highlights: motion takes the highlight to the
 * pointer, and the left button coming up ends it, telling what it
 * highlighted as xterm's EndExtend does: CSI t and where it ends when it
 * starts where the run's first click started and ends at the pointer, and
 * otherwise CSI T and its start, its end and the pointer; nothing when
 * nothing is highlighted. The primary then takes the highlight, as a
 * drag's release fills it, and the next click goes on from the release
 * (vterm_mouse_ended). False when that waits for room. A drag whose screen
 * no longer shows, or whose program no longer tracks (?1001 reset, or a
 * RIS), goes no further, and its change is a normal event's, told only in
 * the mode now set. */
static bool vterm_mouse_track_change(const vterm_mouse_event_t *e)
{
    const vterm_mouse_view_t v = vterm_mouse_view();
    if (!vterm_placement.placed || vterm_mouse_gesture.screen != v.code ||
        vterm.mouse_event != TERM_MOUSE_HILITE)
    {
        vterm_mouse_track_off();
        return true;
    }
    const bool up = !(e->flags & TABLET_FLAG_LEFT);
    const vterm_mouse_pos_t at = vterm_mouse_track_at(&v, e->x, e->y);
    /* The highlight's ends are cells, as xterm's reports name them: the
     * pointer is at the gap before the cell it is over. */
    if ((e->flags & VTERM_MOUSE_PRESENT) && (e->moved || up))
        vterm_mouse_drag(&v, vterm_mouse_cell(&v, at));
    if (!up)
        return true;
    vterm_mouse_pos_t start, end;
    vterm_mouse_span(&v, &start, &end);
    if (vterm_mouse_cmp(start, end))
    {
        const vterm_mouse_pos_t raw = vterm_mouse_pos(&v, vterm_mouse_gesture.raw);
        const uint8_t pos[6] = {(uint8_t)start.x, (uint8_t)start.y, (uint8_t)end.x,
                                (uint8_t)end.y, (uint8_t)at.x, (uint8_t)at.y};
        const bool short_form = !vterm_mouse_cmp(raw, start) && !vterm_mouse_cmp(at, end);
        char out[VTMOUSE_HILITE_MAX];
        const uint8_t n = short_form ? vtmouse_hilite(out, vterm.mouse_form, pos + 2, 2)
                                     : vtmouse_hilite(out, vterm.mouse_form, pos, 6);
        if (!vterm_mouse_type(out, n))
            return false;
    }
    vterm_mouse_ended(true);
    vterm_mouse_keep(&v, CLIP_PRIMARY);
    vterm_mouse_track_off();
    vterm_mouse_flags &= (uint8_t)~TABLET_FLAG_LEFT;
    return true;
}

/* ---- the DEC locator ----------------------------------------------------
 *
 * As xterm's DECterm locator: DECLRP reports, CSI event ; buttons ; row ;
 * col & w, of the buttons the program chose with DECSLE, of a DECRQLP, and
 * of the locator leaving the filter rectangle of a DECEFR. The positions
 * are cells, or pixels with DECELR's Pu 1, from 1, of the screen showing; a
 * locator outside mode 0, or with no pointer, is unavailable. A one-shot
 * locator goes off with its first report, and a button told where the
 * locator is while the filter rectangle is armed turns the locator off, as
 * DEC's; one told unavailable leaves it on, as xterm's SendLocatorPosition.
 * The settings are the console's terminal's (term_locator_t). */

/* Where the locator is, from 1: false when it is unavailable. */
static bool vterm_mouse_locator_at(uint16_t *row, uint16_t *col)
{
    const vterm_placement_t *p = &vterm_placement;
    int y = vterm_mouse_y - p->begin, x = vterm_mouse_x;
    const int height = p->rows * p->row_px, width = p->cols * 8;
    if (!p->placed || !vterm_mouse_here || !tablet_shared() || x < 0 ||
        y < 0 || x >= width || y >= height)
        return false;
    if (!vterm.locator.pixels)
    {
        y /= p->row_px;
        x /= 8;
    }
    *row = (uint16_t)(y + 1);
    *col = (uint16_t)(x + 1);
    return true;
}

/* The buttons held, as DECLRP's mask. */
static uint8_t vterm_mouse_locator_buttons(void)
{
    uint8_t mask = 0;
    for (size_t i = 0; i < VTERM_MOUSE_BUTTON_COUNT; i++)
        if (vterm_mouse_buttons[i].locator != VTERM_MOUSE_NO_LOCATOR &&
            (vterm_mouse_flags & vterm_mouse_buttons[i].flag))
            mask |= VTMOUSE_LOCATOR_MASK(vterm_mouse_buttons[i].locator);
    return mask;
}

/* The locator goes off, its filter rectangle and its events with it. */
static void vterm_mouse_locator_off(void)
{
    vterm.mouse_event = TERM_MOUSE_OFF;
    vterm.locator.filter = false;
    vterm.locator.events = 0;
}

/* A DECLRP of the event where the locator is, unavailable where it is not,
 * typed whole: false, with nothing typed, when there is no room. */
static bool vterm_mouse_locator_send(uint8_t event)
{
    uint16_t row = 0, col = 0;
    if (!vterm_mouse_locator_at(&row, &col))
        event = VTMOUSE_LOCATOR_UNAVAILABLE;
    char out[VTMOUSE_LOCATOR_MAX];
    const uint8_t n = vtmouse_locator(out, event, vterm_mouse_locator_buttons(), row, col);
    return vterm_mouse_type(out, n);
}

/* A report goes out: a one-shot locator goes off with it, as xterm's
 * locator_reset, which turns off whatever mouse events are set. That is so
 * even after the locator went off, as DECELR 0 leaves the one-shot setting:
 * a DECRQLP then answered unavailable turns off a mode set since, as
 * xterm's GetLocatorPosition does. */
static void vterm_mouse_locator_told(void)
{
    if (vterm.locator.once)
        vterm.mouse_event = TERM_MOUSE_OFF;
}

/* A report the terminal asked for, typed now when there is room and none
 * waits before it, and otherwise by the task when there is. It counts as
 * told as it is asked for, so a mode the program sets while it waits is
 * not turned off. */
static void vterm_mouse_locator_due(uint8_t event)
{
    vterm_mouse_locator_told();
    if (!vterm_mouse_dues && vterm_mouse_locator_send(event))
        return;
    if (vterm_mouse_dues < VTERM_MOUSE_DUE)
        vterm_mouse_due[vterm_mouse_dues++] = event;
}

void vterm_mouse_locate(void)
{
    if (vterm.locator.filter)
        vterm_mouse_locator_off();
    if (vterm.mouse_event == TERM_MOUSE_LOCATOR)
        vterm_mouse_locator_due(VTMOUSE_LOCATOR_REQUEST);
    else
        vterm_mouse_locator_due(VTMOUSE_LOCATOR_UNAVAILABLE);
}

/* Whether the locator at row, col is outside the filter rectangle, or the
 * rectangle has no edges of its own, which it then leaves as soon as the
 * locator is somewhere. */
static bool vterm_mouse_locator_outside(uint16_t row, uint16_t col)
{
    const term_locator_t *loc = &vterm.locator;
    return !loc->top || row < loc->top || row > loc->bottom || col < loc->left ||
           col > loc->right;
}

void vterm_mouse_filter(void)
{
    term_locator_t *loc = &vterm.locator;
    uint16_t row, col;
    if (!vterm_mouse_locator_at(&row, &col))
    {
        /* With an edge given, unavailable is told at once; with none, the
         * locator coming back is. */
        if (loc->top || loc->left || loc->bottom || loc->right)
            vterm_mouse_locator_due(VTMOUSE_LOCATOR_UNAVAILABLE);
        else
            loc->filter = true;
        return;
    }
    /* Each edge is held to xterm's limits: the last pixel, or in cells its
     * max_row and max_col, which from 1 are the row and column before the
     * last, so the locator on the last row or column is outside. */
    const vterm_placement_t *p = &vterm_placement;
    const uint16_t rows = loc->pixels ? (uint16_t)(p->rows * p->row_px) : (uint16_t)(p->rows - 1);
    const uint16_t cols = loc->pixels ? (uint16_t)(p->cols * 8) : (uint16_t)(p->cols - 1);
    uint16_t *const edge[4] = {&loc->top, &loc->left, &loc->bottom, &loc->right};
    for (uint8_t i = 0; i < 4; i++)
    {
        const uint16_t here = i & 1 ? col : row, most = i & 1 ? cols : rows;
        *edge[i] = !*edge[i] ? here : *edge[i] > most ? most : *edge[i];
    }
    if (loc->top > loc->bottom)
    {
        const uint16_t was = loc->top;
        loc->top = loc->bottom;
        loc->bottom = was;
    }
    if (loc->left > loc->right)
    {
        const uint16_t was = loc->left;
        loc->left = loc->right;
        loc->right = was;
    }
    if (vterm_mouse_locator_outside(row, col))
        vterm_mouse_locator_due(VTMOUSE_LOCATOR_OUTSIDE);
    else
        loc->filter = true;
}

/* The locator's reports of a change the program takes: leaving the filter
 * rectangle as the locator moves, then each button that went down or up
 * that the program chose. False when one waits for room: vterm_mouse_flags
 * then holds the buttons told so far. */
static bool vterm_mouse_locator_report(const vterm_mouse_event_t *e)
{
    term_locator_t *loc = &vterm.locator;
    uint16_t row, col;
    if (e->moved && loc->filter &&
        (!vterm_mouse_locator_at(&row, &col) || vterm_mouse_locator_outside(row, col)))
    {
        if (!vterm_mouse_locator_send(VTMOUSE_LOCATOR_OUTSIDE))
            return false;
        loc->filter = false;
        vterm_mouse_locator_told();
    }
    for (size_t i = 0; i < VTERM_MOUSE_BUTTON_COUNT; i++)
    {
        const uint8_t flag = vterm_mouse_buttons[i].flag;
        if (!((vterm_mouse_flags ^ e->flags) & flag))
            continue;
        const bool down = e->flags & flag;
        const uint8_t chosen = down ? TERM_LOCATOR_DOWN : TERM_LOCATOR_UP;
        if (vterm.mouse_event == TERM_MOUSE_LOCATOR && (loc->events & chosen) &&
            vterm_mouse_buttons[i].locator != VTERM_MOUSE_NO_LOCATOR)
        {
            /* The mask is the buttons as they are after it. */
            vterm_mouse_flags ^= flag;
            const uint8_t event =
                (uint8_t)(VTMOUSE_LOCATOR_DOWN + 2 * vterm_mouse_buttons[i].locator + !down);
            const bool here = vterm_mouse_locator_at(&row, &col);
            const bool sent = vterm_mouse_locator_send(event);
            vterm_mouse_flags ^= flag;
            if (!sent)
                return false;
            vterm_mouse_locator_told();
            if (here && loc->filter)
                vterm_mouse_locator_off();
        }
        vterm_mouse_flags ^= flag;
    }
    vterm_mouse_flags = e->flags;
    return true;
}

/* ---- the mouse's events -------------------------------------------------- */

/* The reports of a change the program takes: motion, then each button that
 * went down or up, then the wheel's notches. False when one waits for room:
 * vterm_mouse_flags then holds the buttons told so far, and the change the
 * notches still to tell, so the next pass tells the rest. */
static bool vterm_mouse_report(vterm_mouse_event_t *e)
{
    if (vterm_mouse_tracking == VTERM_MOUSE_TRACK_ON && !vterm_mouse_track_change(e))
        return false;
    const uint8_t mode = vterm.mouse_event;
    if (!vterm_placement.placed || !vterm_mouse_asked(e))
    {
        if (!(e->flags & TABLET_FLAG_LEFT))
            vterm_mouse_tracking = VTERM_MOUSE_TRACK_OFF;
        vterm_mouse_flags = e->flags;
        return true;
    }
    if (mode == TERM_MOUSE_LOCATOR)
        return vterm_mouse_locator_report(e);
    const vterm_mouse_spot_t at = vterm_mouse_spot(e->x, e->y);
    const uint8_t held = vterm_mouse_flags & VTERM_MOUSE_BUTTONS;
    if (e->moved && (mode == TERM_MOUSE_ANY || (mode == TERM_MOUSE_BUTTON && held)))
    {
        const bool pixels = vterm.mouse_form == TERM_MOUSE_FORM_PIXELS;
        uint8_t button = VTMOUSE_NONE;
        for (size_t i = VTERM_MOUSE_BUTTON_COUNT; i--;)
            if (held & vterm_mouse_buttons[i].flag)
                button = vterm_mouse_buttons[i].button;
        if ((pixels ? at.x : at.col) != vterm_mouse_sent_x ||
            (pixels ? at.y : at.row) != vterm_mouse_sent_y)
            if (!vterm_mouse_send(VTMOUSE_MOTION, button, e->modifier, &at))
                return false;
    }
    for (size_t i = 0; i < VTERM_MOUSE_BUTTON_COUNT; i++)
    {
        const uint8_t flag = vterm_mouse_buttons[i].flag;
        if (!((vterm_mouse_flags ^ e->flags) & flag))
            continue;
        const bool down = e->flags & flag;
        const bool track = flag == TABLET_FLAG_LEFT && down && mode == TERM_MOUSE_HILITE;
        const uint8_t event = !down                    ? VTMOUSE_RELEASE
                              : mode == TERM_MOUSE_X10 ? VTMOUSE_X10
                                                       : VTMOUSE_PRESS;
        if (!(track && vterm_mouse_track_down(e)))
        {
            if ((down || mode != TERM_MOUSE_X10) &&
                !vterm_mouse_send(event, vterm_mouse_buttons[i].button, e->modifier, &at))
                return false;
            /* Highlight tracking's press waits for the program's answer
             * until it comes up. */
            if (track)
                vterm_mouse_track_wait(e);
            else if (flag == TABLET_FLAG_LEFT)
                vterm_mouse_tracking = VTERM_MOUSE_TRACK_OFF;
        }
        /* By the selection's rule, a left click counts on from the last
         * only when no other button went down or came up between them;
         * highlight tracking's (vterm_mouse_track_down) does not ask. */
        if (flag != TABLET_FLAG_LEFT)
            vterm_mouse_gesture.clicked = false;
        vterm_mouse_flags ^= flag;
    }
    if (mode != TERM_MOUSE_X10)
    {
        while (e->wheel)
        {
            if (!vterm_mouse_send(VTMOUSE_PRESS, e->wheel > 0 ? VTMOUSE_WHEEL_UP : VTMOUSE_WHEEL_DOWN,
                                  e->modifier, &at))
                return false;
            e->wheel = (int8_t)(e->wheel > 0 ? e->wheel - 1 : e->wheel + 1);
        }
        while (e->pan)
        {
            if (!vterm_mouse_send(VTMOUSE_PRESS, e->pan > 0 ? VTMOUSE_WHEEL_RIGHT : VTMOUSE_WHEEL_LEFT,
                                  e->modifier, &at))
                return false;
            e->pan = (int8_t)(e->pan > 0 ? e->pan - 1 : e->pan + 1);
        }
    }
    vterm_mouse_flags = e->flags;
    return true;
}

/* ---- alternate scroll ---------------------------------------------------- */

/* The cursor keys a notch of the wheel presses, as xterm's scroll-back(5,
 * line) and scroll-forw(5,line). */
#define VTERM_MOUSE_SCROLL_KEYS 5

/* With alternate scroll over the alternate screen and no mouse mode, a
 * notch of the wheel up presses the up arrow, and down the down arrow,
 * VTERM_MOUSE_SCROLL_KEYS times, with no modifiers, as the cursor keys
 * send them in the modes the terminal is in (vtkey.h). Otherwise the wheel
 * does nothing. False while a notch waits for room, with the change
 * holding the notches still to press. */
static bool vterm_mouse_scroll(vterm_mouse_event_t *e)
{
    if (!vterm_mouse_alt_scrolls())
        return true;
    while (e->wheel)
    {
        char key[VTKEY_MAX];
        const int n = vtkey_encode(key, e->wheel > 0 ? HID_KEY_ARROW_UP : HID_KEY_ARROW_DOWN, 0);
        if (n <= 0)
            break;
        if ((size_t)n * VTERM_MOUSE_SCROLL_KEYS > typed_free())
            return false;
        for (int i = 0; i < VTERM_MOUSE_SCROLL_KEYS; i++)
            typed_push(key, (size_t)n);
        e->wheel = (int8_t)(e->wheel > 0 ? e->wheel - 1 : e->wheel + 1);
    }
    return true;
}

/* ---- focus --------------------------------------------------------------- */

void vterm_focus(bool focused)
{
    vterm_focus_now = focused;
}

bool vterm_focused(void)
{
    return vterm_focus_now;
}

/* A change of the focus is told, CSI I gained and CSI O lost, while the
 * program asks (?1004). Without, the program was told nothing, and setting
 * the mode later tells nothing until the focus changes, as xterm. False
 * while it waits for room. */
static bool vterm_focus_tell(void)
{
    if (!vterm.focus_events)
        vterm_focus_told = vterm_focus_now;
    if (vterm_focus_told == vterm_focus_now)
        return true;
    if (!vterm_mouse_type(vterm_focus_now ? "\33[I" : "\33[O", 3))
        return false;
    vterm_focus_told = vterm_focus_now;
    return true;
}

/* ---- the task ------------------------------------------------------------ */

/* The locator reports the terminal asked for go first, then a change of the
 * focus, then the changes, in order. One the program takes, whose report
 * waits for room, stays first in the queue, where only motion merges into
 * it, and the rest wait behind it. While a line is read, one the terminal
 * takes waits there the same way, for the room the most it can queue
 * needs, before anything of it is done. With none read, only the middle
 * button's paste is queued, and a full queue drops it, as it drops a key. */
void HOST_IN_FLASH_FUNC vterm_mouse_task(void)
{
    for (; vterm_mouse_dues; vterm_mouse_dues--)
    {
        if (!vterm_mouse_locator_send(vterm_mouse_due[0]))
            return;
        for (uint8_t i = 1; i < vterm_mouse_dues; i++)
            vterm_mouse_due[i - 1] = vterm_mouse_due[i];
    }
    if (!vterm_focus_tell())
        return;
    while (RING_LEN(&vterm_mouse_queue))
    {
        vterm_mouse_event_t *e = RING_AT(&vterm_mouse_queue, 0);
        const uint8_t was = vterm_mouse_flags;
        vterm_mouse_x = e->x;
        vterm_mouse_y = e->y;
        vterm_mouse_here = e->flags & VTERM_MOUSE_PRESENT;
        /* With no button held, this change decides, a press included; a
         * release goes where its press went. */
        if (!(was & VTERM_MOUSE_BUTTONS))
        {
            vterm_mouse_acting = vterm_mouse_takes(e);
            vterm_mouse_reporting = vterm_mouse_acting && vterm_mouse_asked(e);
        }
        if (vterm_mouse_reporting)
        {
            if (!vterm_mouse_report(e))
                return;
        }
        else
        {
            if (vterm_mouse_acting &&
                (!vterm_mouse_scroll(e) || (rln_reading() && typed_free() < VTERM_MOUSE_ENTRIES)))
                return;
            vterm_mouse_flags = e->flags;
            if (vterm_mouse_acting)
                vterm_mouse_act(e, was);
        }
        RING_SKIP(&vterm_mouse_queue, 1);
    }
}
