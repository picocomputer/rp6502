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
#include "core/hid/usage.h"
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
 * otherwise at every change. */
static uint8_t vterm_mouse_flags;
static bool vterm_mouse_acting;

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
 * and a drag after them goes by words and by lines; a fourth selects by
 * characters again. The right button, and Shift with the left, extend the
 * selection from its nearer end, in the unit it was made in. The release
 * fills the primary with what is selected. The middle button pastes the
 * primary as it comes up.
 *
 * The points are the store's marks (screen.h), on the screen the gesture
 * is on: rows of memory, as the selection is, so output that scrolls the
 * screen during a drag keeps them on their text, and one whose row leaves
 * the screen goes to the edge it left by, as xterm's do. */

#define VTERM_MOUSE_CLICK_MS 250

enum
{
    VTERM_MOUSE_CHAR,
    VTERM_MOUSE_WORD,
    VTERM_MOUSE_LINE,
};

/* The console's widest grid. */
#define VTERM_MOUSE_MAX_COLS 80

/* A point of a screen: a row of memory, and a column of that row's own. */
typedef screen_mark_t vterm_mouse_cell_t;

/* The points the gesture holds, which the store keeps in step as the
 * marks, an array of them. */
#define VTERM_MOUSE_POINTS 5

typedef struct
{
    uint8_t screen; // the screen it is on: bit 1 the 80-column grid, bit 0 the alternate
    uint8_t button; // TABLET_FLAG_LEFT or _RIGHT while its drag goes on, else 0
    uint8_t unit;   // VTERM_MOUSE_CHAR, _WORD or _LINE
    uint8_t clicks; // the left button's clicks in a row, 1 to 3, or 0 before any
    bool left;      // the drag moves the start, xterm's LEFTEXTENSION
    bool clicked;   // the last press was a left click that counts, and no other button went down or up since
    bool line;      // the drag selects in the line editor's line (core/com/rln.h)
    union
    {
        struct
        {
            vterm_mouse_cell_t ext_start, ext_end;   // what is selected between, as the unit has not widened it
            vterm_mouse_cell_t save_start, save_end; // ext_start and ext_end as the drag began
            vterm_mouse_cell_t click;                // where the left button last went down
        };
        vterm_mouse_cell_t points[VTERM_MOUSE_POINTS]; // the same, for the store
    };
    timer_mach_t click_by; // a press there before this is another click
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
    const vterm_mouse_gesture_t *gs = &vterm_mouse_gesture;
    if (gs->screen > 3 || gs->unit > VTERM_MOUSE_LINE || gs->clicks > 3 ||
        (gs->button & ~(TABLET_FLAG_LEFT | TABLET_FLAG_RIGHT)) || (gs->button && !gs->clicks) ||
        (gs->line && gs->button != TABLET_FLAG_LEFT))
        return sst_refuse(c, "a mouse gesture off the console's screens");
    if (gs->clicks)
    {
        const term_grid_t *g = gs->screen >> 1 ? &vterm_80.grid : &vterm_40.grid;
        const screen_t *s = vterm_mouse_screen(gs->screen);
        if (!s->cells)
            return sst_refuse(c, "a mouse gesture on a screen with no cells");
        for (unsigned i = 0; i < VTERM_MOUSE_POINTS; i++)
            if (!vterm_mouse_shown_row(g, s, gs->points[i].row) ||
                gs->points[i].col > VTERM_MOUSE_MAX_COLS)
                return sst_refuse(c, "a mouse gesture off its screen's rows");
    }
    vterm_mouse_mark();
    return true;
}

#define VTERM_MOUSE_EVENT_ROWS(Y, e)                         \
    Y(I16, e.x) Y(I16, e.y) Y(U8, e.flags) Y(U8, e.modifier) \
    Y(U8, e.wheel) Y(U8, e.pan) Y(BOOL, e.edge)              \
    Y(BOOL, e.moved) Y(BOOL, e.shared)
#define VTERM_MOUSE_CELL_ROWS(Y, p) Y(U8, p.row) Y(U8, p.col)
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
    X(STRUCT, vterm_mouse_gesture.ext_start, VTERM_MOUSE_CELL_ROWS)           \
    X(STRUCT, vterm_mouse_gesture.ext_end, VTERM_MOUSE_CELL_ROWS)             \
    X(STRUCT, vterm_mouse_gesture.save_start, VTERM_MOUSE_CELL_ROWS)          \
    X(STRUCT, vterm_mouse_gesture.save_end, VTERM_MOUSE_CELL_ROWS)            \
    X(STRUCT, vterm_mouse_gesture.click, VTERM_MOUSE_CELL_ROWS)               \
    X(U64, vterm_mouse_gesture.click_by)                                      \
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
    vterm_mouse_gesture = (vterm_mouse_gesture_t){0};
    vterm_mouse_mark();
}

bool tablet_claimed(void)
{
    return vterm_placement.placed;
}

/* The terminal has no scrollback, so the wheel does nothing here. */
bool tablet_wheel_claimed(void)
{
    return false;
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

/* The cell under canvas pixel x, y, mode 0 placed: a pointer above or below
 * mode 0's rows is on its first or last row, and one past the right of a
 * row is one past its last column. A row of double width has half the
 * columns, 16 pixels each. */
static vterm_mouse_pos_t vterm_mouse_at(const vterm_mouse_view_t *v, int x, int y)
{
    const vterm_placement_t *p = &vterm_placement;
    vterm_mouse_pos_t at = {y < p->begin ? 0 : (y - p->begin) / p->row_px, 0};
    if (at.y >= v->g->height)
        at.y = v->g->height - 1;
    const bool wide = screen_size(v->s, (uint8_t)at.y);
    const int cols = wide ? v->g->width / 2 : v->g->width;
    at.x = x < 0 ? 0 : x / (wide ? 16 : 8);
    if (at.x > cols)
        at.x = cols;
    return at;
}

static vterm_mouse_cell_t vterm_mouse_cell(const vterm_mouse_view_t *v, vterm_mouse_pos_t p)
{
    return (vterm_mouse_cell_t){screen_mem(v->s, (uint8_t)p.y), (uint8_t)p.x};
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

static bool vterm_mouse_cell_eq(vterm_mouse_cell_t a, vterm_mouse_cell_t b)
{
    return a.row == b.row && a.col == b.col;
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

/* xterm's okPosition: a point past a row's text, but on its last row, is
 * the start of the next row. False when it moved. */
static bool vterm_mouse_settle(vterm_mouse_line_t *l, vterm_mouse_pos_t *p)
{
    if (p->x <= l->last + 1 || !vterm_mouse_more(l->v, p->y))
        return true;
    p->x = 0;
    vterm_mouse_load(l, ++p->y);
    return false;
}

/* What is selected between two points, as the unit widens them, as xterm's
 * ComputeSelect: by words, out to the ends of the words under them, across
 * a row that wrapped; by lines, from the first row of the line under the
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
        vterm_mouse_settle(&ls, start);
        vterm_mouse_settle(&le, end);
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

/* The selection becomes what the gesture selects. One that is already so,
 * showing or empty, is left as it is, so the RIA sends only a change. */
static void vterm_mouse_compute(const vterm_mouse_view_t *v)
{
    vterm_mouse_pos_t start, end;
    vterm_mouse_span(v, &start, &end);
    const vterm_mouse_cell_t a = vterm_mouse_cell(v, start), b = vterm_mouse_cell(v, end);
    const screen_selection_t *sel = &vterm_selection;
    if (sel->screen == v->s && sel->start_row == a.row && sel->start_col == a.col &&
        sel->end_row == b.row && sel->end_col == b.col &&
        (sel->shows || vterm_mouse_cell_eq(a, b)))
        return;
    vterm_select(v->g, v->code & 1, a.row, a.col, b.row, b.col);
}

/* A press at the cell, held by button: its clicks in a row, which a press
 * that is counted adds to, and the unit they make. */
static vterm_mouse_cell_t vterm_mouse_press(const vterm_mouse_view_t *v, vterm_mouse_pos_t at,
                                            uint8_t button, bool counted)
{
    const vterm_mouse_cell_t c = vterm_mouse_cell(v, at);
    if (counted && vterm_mouse_gesture.clicked && vterm_mouse_gesture.screen == v->code &&
        vterm_mouse_cell_eq(c, vterm_mouse_gesture.click) &&
        !timer_mach_passed(vterm_mouse_gesture.click_by))
        vterm_mouse_gesture.clicks = vterm_mouse_gesture.clicks % 3 + 1;
    else
        vterm_mouse_gesture.clicks = 1;
    vterm_mouse_gesture.unit = (uint8_t)(vterm_mouse_gesture.clicks - 1);
    vterm_mouse_gesture.clicked = counted;
    vterm_mouse_gesture.click = c;
    vterm_mouse_gesture.screen = v->code;
    vterm_mouse_mark();
    vterm_mouse_gesture.button = button;
    vterm_mouse_gesture.line = false;
    return c;
}

/* A press starts a selection at the cell, by the unit its clicks make, and
 * the line editor's ends: one selection at a time. */
static void vterm_mouse_start(const vterm_mouse_view_t *v, vterm_mouse_pos_t at, uint8_t button,
                              bool counted)
{
    const vterm_mouse_cell_t c = vterm_mouse_press(v, at, button, counted);
    rln_unselect();
    vterm_mouse_gesture.left = false;
    vterm_mouse_gesture.ext_start = vterm_mouse_gesture.ext_end = c;
    vterm_mouse_gesture.save_start = vterm_mouse_gesture.save_end = c;
    vterm_mouse_compute(v);
}

/* A press of the right button, or Shift with the left, takes the
 * selection's nearer end to the cell, as xterm's start-extend, in the unit
 * it was made in. With no selection showing there, it selects from the
 * cell as the left button does. */
static void vterm_mouse_extend(const vterm_mouse_view_t *v, vterm_mouse_pos_t at, uint8_t button)
{
    if (!vterm_selection.shows || vterm_selection.screen != v->s ||
        vterm_mouse_gesture.screen != v->code)
    {
        vterm_mouse_start(v, at, button, false);
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
    if (vterm_mouse_gesture.left)
        vterm_mouse_gesture.ext_start = vterm_mouse_cell(v, at);
    else
        vterm_mouse_gesture.ext_end = vterm_mouse_cell(v, at);
    vterm_mouse_compute(v);
}

/* A drag takes the end it moves to the cell, as xterm's ExtendExtend: one
 * that crosses the other end turns round, and the other end goes back to
 * where it was as the drag began. */
static void vterm_mouse_drag(const vterm_mouse_view_t *v, vterm_mouse_pos_t at)
{
    vterm_mouse_pos_t start, end;
    vterm_mouse_span(v, &start, &end);
    if (vterm_mouse_gesture.left &&
        vterm_mouse_cmp(at, end) >= (vterm_mouse_gesture.unit == VTERM_MOUSE_CHAR))
    {
        vterm_mouse_gesture.left = false;
        vterm_mouse_gesture.ext_start = vterm_mouse_gesture.save_start;
    }
    else if (!vterm_mouse_gesture.left && vterm_mouse_cmp(at, start) < 0)
    {
        vterm_mouse_gesture.left = true;
        vterm_mouse_gesture.ext_end = vterm_mouse_gesture.save_end;
    }
    if (vterm_mouse_gesture.left)
        vterm_mouse_gesture.ext_start = vterm_mouse_cell(v, at);
    else
        vterm_mouse_gesture.ext_end = vterm_mouse_cell(v, at);
    vterm_mouse_compute(v);
}

/* The drag ends: the primary takes what it selected, if anything. A
 * selection that shows is the gesture's, on rows of the height. */
static void vterm_mouse_keep(const vterm_mouse_view_t *v)
{
    if (!vterm_selection.shows || vterm_selection.screen != v->s)
        return;
    const screen_selection_t *sel = &vterm_selection;
    const vterm_mouse_pos_t start =
        vterm_mouse_pos(v, (vterm_mouse_cell_t){sel->start_row, sel->start_col});
    const vterm_mouse_pos_t end =
        vterm_mouse_pos(v, (vterm_mouse_cell_t){sel->end_row, sel->end_col});
    clip_copy(CLIP_PRIMARY, v->s, (uint8_t)start.y, (uint8_t)start.x, (uint8_t)end.y,
              (uint8_t)end.x);
}

/* The drag goes no further. */
static void vterm_mouse_let_go(void)
{
    vterm_mouse_gesture.button = 0;
    vterm_mouse_gesture.line = false;
}

/* A press of the left button on the line the line editor is editing, at
 * index of the line, selects in the line, judged against the line as it
 * is laid out now, and ends the primary's highlight. */
static void vterm_mouse_line(const vterm_mouse_view_t *v, vterm_mouse_pos_t at, uint8_t index)
{
    vterm_mouse_press(v, at, TABLET_FLAG_LEFT, true);
    vterm_mouse_gesture.line = true;
    if (vterm_selection.shows)
        vterm_unselect();
    rln_mouse_press(index, vterm_mouse_gesture.clicks);
}

/* A change the terminal took, with the buttons that were down before it. */
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
    /* A drag on a screen no longer showing goes no further. */
    if (vterm_mouse_gesture.button && vterm_mouse_gesture.screen != v.code)
        vterm_mouse_let_go();
    /* A contact that has gone, as a finger lifted, is nowhere, so its
     * release ends the drag where it last was. A drag in the line editor's
     * line stays in the line, and the line's selection fills no primary. */
    if (vterm_mouse_gesture.button && (e->flags & VTERM_MOUSE_PRESENT) &&
        (e->moved || (up & vterm_mouse_gesture.button)))
    {
        if (vterm_mouse_gesture.line)
            rln_mouse_drag((uint8_t)at.y, (uint8_t)at.x);
        else
            vterm_mouse_drag(&v, at);
    }
    if (up & vterm_mouse_gesture.button)
    {
        if (!vterm_mouse_gesture.line)
            vterm_mouse_keep(&v);
        vterm_mouse_let_go();
    }
    /* A left click counts on from the last only when no other button went
     * down or came up between them, and a press that extends counts none
     * (vterm_mouse_extend). */
    if ((down | up) & ~TABLET_FLAG_LEFT)
        vterm_mouse_gesture.clicked = false;
    if (up & TABLET_FLAG_LEFT)
        vterm_mouse_gesture.click_by = timer_mach_in_ms(VTERM_MOUSE_CLICK_MS);
    if (up & TABLET_FLAG_MIDDLE)
        clip_paste(CLIP_PRIMARY);
    if (vterm_mouse_gesture.button)
        return;
    const bool shift = e->modifier & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT);
    const int index = (down & TABLET_FLAG_LEFT) && !shift ? rln_index_at((uint8_t)at.y, (uint8_t)at.x)
                                                           : -1;
    if (index >= 0)
        vterm_mouse_line(&v, at, (uint8_t)index);
    else if ((down & TABLET_FLAG_LEFT) && !shift)
        vterm_mouse_start(&v, at, TABLET_FLAG_LEFT, true);
    else if (down & TABLET_FLAG_LEFT)
        vterm_mouse_extend(&v, at, TABLET_FLAG_LEFT);
    else if (down & TABLET_FLAG_RIGHT)
        vterm_mouse_extend(&v, at, TABLET_FLAG_RIGHT);
}

void HOST_IN_FLASH_FUNC vterm_mouse_task(void)
{
    vterm_mouse_event_t e;
    while (RING_GET(&vterm_mouse_queue, &e))
    {
        const uint8_t was = vterm_mouse_flags;
        /* With no button held, this change decides, a press included; a
         * release goes where its press went. */
        if (!(was & VTERM_MOUSE_BUTTONS))
            vterm_mouse_acting = vterm_mouse_takes(&e);
        vterm_mouse_flags = e.flags;
        if (vterm_mouse_acting)
            vterm_mouse_act(&e, was);
    }
}
