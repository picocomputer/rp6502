/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_TERM_TERM_H_
#define _CORE_TERM_TERM_H_

/* The terminal: a VT102 with most of what xterm adds. Bytes go in
 * with term_write and answers to queries come out through cells_reply (cells.h).
 *
 * One terminal follows one stream, and it can be followed at several sizes
 * at once: the console's 40 and 80 columns, and on the RIA a program's own
 * size. Each size is a grid (term_grid_t) with its own cursor position,
 * margins, tab stops and line sizes. Everything else is the same at every
 * size and is kept once in term_t: the parser, every mode, the pens and the
 * saved pens, the character sets and which screen is showing. So a byte is
 * parsed once, and only what moves the cursor or the rows runs once a grid.
 *
 * The terminal holds no cells and no palette. It changes the screen, and
 * answers queries, through the functions in cells.h, which cells.c defines
 * on a machine that draws the screen; a machine that only follows the
 * terminal's state defines them itself. */

#include "core/sys/sst.h"
#include "machine.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* TERM_ALT_SCREEN: the ?47 / ?1047 / ?1049 alternate screen.
 *
 * It doubles the cell memory, and on a platform whose cells live in FPGA
 * block memory that is the difference between a feature and a firmware.
 * On an FPGA that cannot spare the blocks, it can be compiled out and the
 * terminal still builds, which is why this is a switch and not a deletion.
 *
 * Off, the escape sequences still parse and the cursor still saves and
 * restores — only the screen swap is skipped, so a full-screen program
 * draws on the primary and leaves its output behind on exit. That is
 * what a terminal without an alternate screen has always done. A machine
 * that follows another's terminal has to use the same setting. */
#ifndef TERM_ALT_SCREEN
#define TERM_ALT_SCREEN 1
#endif

/* TERM_SPARE_ROWS: the row of memory each screen keeps beyond its
 * TERM_MAX_HEIGHT for DECSCLM, which holds the row a smooth scroll moves
 * out while it moves. A machine whose cell store is fixed elsewhere, as
 * in an FPGA, can leave it out with 0, and the terminal there scrolls
 * whole rows even in smooth scroll mode. */
#ifndef TERM_SPARE_ROWS
#define TERM_SPARE_ROWS 1
#endif
#define TERM_MEM_ROWS (TERM_MAX_HEIGHT + TERM_SPARE_ROWS)

/* A smooth scroll moves a row in this many frames: one scanline a frame on
 * the 16-scanline rows, one every second frame on the 8-scanline rows of
 * the 40-column terminal, so both move a line in the same time. */
#define TERM_SMOOTH_FRAMES 16

/* The widest grid there can be. A screen's is 40 or 80; a program can make
 * the terminal wider than its screen. A grid's height is up to its rows. */
#define TERM_MAX_WIDTH 255
#define TERM_TAB_BITMAP_BYTES ((TERM_MAX_WIDTH + 7) / 8)
/* A grid's height until its machine says otherwise: the 40-column
 * terminal's tallest, and the 80-column terminal's on every display but
 * SXGA. */
#define TERM_STD_HEIGHT 30
/* The sizes one terminal can be followed at: the screen's 40 and 80
 * columns, and a program's own size (RIA_ATTR_RLN_WIDTH and HEIGHT). */
#define TERM_SCREEN_GRIDS 2
#define TERM_MAX_GRIDS 3
/* 16 parameters, as Linux's NPAR. More are dropped; the sequence still runs. */
#define TERM_CSI_PARAM_MAX_LEN 16
/* One OSC item, such as "rgb:ffff/ffff/ffff", or the bytes of a DCS string.
 * Longer ones are ignored. */
/* The colors one OSC can ask for. They are answered when the OSC ends, and
 * this many answers are what the machine holds unread. */
#define TERM_OSC_ASK_MAX 6
#define TERM_OSC_SPEC_MAX 20

// The SGR attributes. All but the italics are also the low bits of a cell's
// attributes (mode0.h); italics are a cell's font. SGR 5 (slow) and SGR 6
// (rapid) live at bits 0-1 so they map onto the live blink phase counter
// directly. A cell carries exactly one of the two; the renderer ANDs it
// against the phase so each pulses at its own rate. See cells.c's
// TERM_BLINK_TICK_FRAMES.
#define TERM_ATTR_BLINK_FAST (1u << 0) // SGR 6 rapid (blink phase bit 0)
#define TERM_ATTR_BLINK (1u << 1)      // SGR 5 slow  (blink phase bit 1)
// The underline's style, a TERM_UL_: SGR 4, kitty's SGR 4:n, and SGR 21.
#define TERM_ATTR_UL_SHIFT 2
#define TERM_ATTR_UL_MASK (7u << TERM_ATTR_UL_SHIFT)
#define TERM_ATTR_UL(style) ((unsigned)(style) << TERM_ATTR_UL_SHIFT)
#define TERM_ATTR_STRIKE (1u << 5)   // SGR 9
#define TERM_ATTR_OVERLINE (1u << 6) // SGR 53
#define TERM_ATTR_ITALIC (1u << 7)   // SGR 3 (ASCII-only, 8x16 mode only)
#define TERM_ATTR_ANY_BLINK (TERM_ATTR_BLINK | TERM_ATTR_BLINK_FAST)
#define TERM_ATTR_CELL_MASK (TERM_ATTR_BLINK | TERM_ATTR_BLINK_FAST | \
                             TERM_ATTR_UL_MASK |                      \
                             TERM_ATTR_STRIKE | TERM_ATTR_OVERLINE)

// The underline styles, numbered as kitty's SGR 4:n.
#define TERM_UL_NONE 0
#define TERM_UL_SINGLE 1
#define TERM_UL_DOUBLE 2
#define TERM_UL_CURLY 3
#define TERM_UL_DOTTED 4
#define TERM_UL_DASHED 5

// A line's size, DEC's line attributes (ESC # 3 to 6). A double-size line
// holds half the columns, each drawn twice as wide; the double-height halves
// draw the top or the bottom half of each glyph, twice as tall.
#define TERM_LINE_NORMAL 0 // ESC # 5
#define TERM_LINE_WIDE 1   // ESC # 6
#define TERM_LINE_TOP 2    // ESC # 3
#define TERM_LINE_BOTTOM 3 // ESC # 4

typedef enum
{
    ansi_state_ground,
    ansi_state_esc,         // after ESC
    ansi_state_esc_int,     // after ESC and an intermediate byte, 0x20-0x2F
    ansi_state_csi,         // after CSI, collecting parameters and intermediates
    ansi_state_csi_ignore,  // a malformed CSI, dropped through its final byte
    ansi_state_osc,         // an OSC string, up to BEL or ST
    ansi_state_string,      // SOS, PM or APC, ignored up to ST
    ansi_state_vt52_esc,    // after ESC in VT52 mode
    ansi_state_vt52_row,    // after ESC Y
    ansi_state_vt52_col,    // after ESC Y and the row
    ansi_state_dcs,         // a DCS string, kept for DECRQSS, up to ST
    ansi_state_count
} ansi_state_t;

// The character sets that G0 and G1 can hold.
#define TERM_CS_ASCII 0 // ESC ( B, and the null and user maps ESC ( U / K
#define TERM_CS_DEC 1   // ESC ( 0, DEC Special Graphics
#define TERM_CS_UK 2    // ESC ( A, ASCII with the pound sign at 0x23

// Where a pen's color comes from. A palette color keeps its index, so the
// cells take a changed palette entry, and the default colors are their own
// kind, apart from palette 7 and 0, so SGR 39/49 are not SGR 37/40 and OSC 4
// never changes the defaults.
#define TERM_COLOR_DEFAULT 0
#define TERM_COLOR_INDEX 1
#define TERM_COLOR_RGB 2

typedef struct
{
    uint8_t kind;    // TERM_COLOR_
    uint8_t index;   // the palette entry, for TERM_COLOR_INDEX
    uint16_t rgb;    // the color as a pixel, for TERM_COLOR_RGB
    uint8_t r, g, b; // the color as SGR gave it, which DECRQSS gives back
} term_color_t;

// The pen: SGR, origin mode and the character sets, what DECSC saves along
// with the cursor's position.
typedef struct
{
    uint8_t sgr_attr; // TERM_ATTR_ bits
    term_color_t fg, bg;
    term_color_t ul; // SGR 58, when ul_set
    bool ul_set;     // SGR 58 active; SGR 59 and SGR 0 clear it
    bool bold;
    bool faint;
    bool reverse; // SGR 7: the cells swap fg and bg
    bool conceal; // SGR 8: the cells draw fg as bg
    bool protect; // DECSCA 1 or SPA: the cells it writes are protected
    bool origin_mode;
    uint8_t g0_charset, g1_charset;
    bool gl_is_g1;
} term_pen_t;

// Which erases leave the protected cells alone: none, as after a reset;
// DECSED and DECSEL alone, after a DECSCA (DEC's selective erase); or those
// and ED, EL and ECH too, after an SPA (ECMA-48's guarded areas), unless ERM
// says erase them all. As xterm, one protected bit serves both, and the
// last of DECSCA and SPA picks which.
#define TERM_PROTECT_OFF 0
#define TERM_PROTECT_DEC 1
#define TERM_PROTECT_ISO 2

// A cursor's place in a grid.
typedef struct
{
    uint8_t x, y;
    bool wrap_pending; // the last column was written; the next glyph wraps
} term_pos_t;

// The terminal at one size. Each screen, primary and alternate, has its own
// cursor position, saved position and line sizes; the margins and the tab
// stops belong to the grid. The cursor's column never reaches the width:
// writing the last column sets wrap_pending instead, as on a VT102. The
// left and right margins are the whole width until DECSLRM sets them.
typedef struct
{
    uint8_t width;
    uint8_t height;
    uint8_t rows; // the most rows: lines holds this many for each screen
    uint8_t margin_top, margin_bot;
    uint8_t margin_left, margin_right;
    term_pos_t pos[2];   // the cursor on each screen
    term_pos_t saved[2]; // DECSC's position on each screen
    uint8_t *lines;      // TERM_LINE_ for each row: rows for the primary, then the alternate
    term_pos_t *cur;     // the showing screen's pos
    uint8_t *ls;         // the showing screen's lines
    uint8_t tab_stops[TERM_TAB_BITMAP_BYTES]; // 1 bit per column
} term_grid_t;

typedef struct
{
    term_pen_t pen[2];   // the pen on each screen
    term_pen_t saved[2]; // DECSC, CSI s, ?1048 and ?1049, on each screen
    bool has_saved[2];   // a DECSC since the last reset, for DECRQM ?1048
    term_pen_t *cur;     // the showing screen's pen
    bool alt_active;     // the alternate screen is showing; term_show sets it
    bool autowrap;        // DECAWM, ?7
    bool insert_mode;     // IRM, CSI 4 h
    bool newline_mode;    // LNM, CSI 20 h: LF, VT and FF also return the carriage
    bool disp_ctrl;       // CRM, CSI 3 h: draw the C0 controls
    bool vt52;            // DECANM reset, ?2 l
    bool app_cursor;      // DECCKM, ?1: the cursor keys send SS3
    bool app_keypad;      // DECKPAM, ESC = or ?66: the keypad sends SS3
    bool autorepeat;      // DECARM, ?8
    bool ice_colors;      // ?33: SGR 5/6 means bright bg (the IBM VGA hack), not blink
    bool bracketed_paste; // ?2004: a paste comes between ESC [ 200 ~ and ESC [ 201 ~
    uint8_t modify_other_keys; // xterm's modifyOtherKeys, 0 to 3: CSI > 4 ; n m
    bool format_other_keys;    // xterm's formatOtherKeys: CSI > 4 ; 1 f
    bool lr_margins;      // DECLRMM, ?69: CSI s sets the left and right margins
    bool rect_attrs;      // DECSACE 2: DECCARA and DECRARA change a rectangle, not a stream
    uint8_t protect_mode; // TERM_PROTECT_
    bool erase_all;       // ERM, CSI 6 h: ED, EL and ECH erase the protected cells too
    bool reverse_screen;  // DECSCNM, ?5: every cell's colors swapped
    bool smooth_scroll;   // DECSCLM, ?4: a line feed or a reverse index moves the rows a scanline a frame
    bool smooth_released; // term_release_smooth: nothing waits for a smooth scroll until DECSCLM is set again
    bool cursor_enabled;  // DECTCEM, ?25
    uint8_t cursor_style; // DECSCUSR Ps: 0/1/2 block, 3/4 underline, 5/6 bar
    uint8_t last_glyph;   // for REP
    bool rep_valid;       // the last thing written was a glyph
    /* The parser. */
    uint8_t ansi_state; // an ansi_state_t
    uint16_t csi_param[TERM_CSI_PARAM_MAX_LEN];
    char csi_separator[TERM_CSI_PARAM_MAX_LEN]; // the byte after each parameter: ';', ':' or 0
    uint8_t csi_param_count;  // the parameter being collected; at dispatch, how many there are
    uint8_t csi_private;      // '<', '=', '>', '?' as the first byte, or 0
    uint8_t csi_intermediate; // the one intermediate of a CSI or ESC sequence, 0, or 0xFF for more
    bool csi_colon;           // a ':' was seen, which only SGR takes
    bool csi_any;             // a parameter byte was seen, so a private marker is too late
    bool osc_pending;         // an ESC arrived inside an OSC, so ESC \ ends it
    bool dcs_pending;         // the same inside a DCS, whose bytes are in osc_spec
    uint16_t osc_ps;          // the OSC command number
    uint8_t osc_stage;        // 0 collecting the number, 1 the items, 2 dropping the string
    uint8_t osc_item;         // how many items of the command are done
    uint16_t osc_index;       // OSC 4: the palette entry the next color is for
    uint8_t osc_len;          // bytes in osc_spec, or more than TERM_OSC_SPEC_MAX when too long
    char osc_spec[TERM_OSC_SPEC_MAX];
    uint8_t osc_asks;         // the colors this OSC has asked for so far
    struct
    {
        uint16_t which; // a palette entry, or CELLS_COLOR_
        uint16_t color; // what it was when it was asked for
    } osc_ask[TERM_OSC_ASK_MAX];
    /* The sizes it is followed at. */
    term_grid_t *grids[TERM_MAX_GRIDS];
    uint8_t grid_count;
} term_t;

// A grid of width columns by 30 rows (fewer if it holds fewer), its line
// sizes in lines, which holds rows bytes for each of the two screens.
void term_grid_init(term_grid_t *g, uint8_t width, uint8_t *lines, uint8_t rows);

// A new terminal followed at the given grids, after a RIS.
void term_init(term_t *t, term_grid_t *const *grids, uint8_t count);

// One byte of the stream.
void term_write(term_t *t, uint8_t ch);

// True when ch must wait for a smooth scroll still moving: it would scroll
// that region again, or set the margins (DECSTBM), as on a VT100. The
// byte is not taken; whoever feeds the terminal holds it and the bytes
// after it, and writes it once this is false. Other bytes go on into the
// rows while they move.
bool term_holds(const term_t *t, uint8_t ch);

// The end of a chain: from here nothing waits for a smooth scroll, so what
// was held goes out at once, and a scroll that reaches one still moving
// snaps it into place. It lasts until DECSCLM is set or reset again, which
// the soft reset at the end of a chain does.
void term_release_smooth(term_t *t);

// RIS, and the system's soft reset: RIS leaving the screen and the cursor.
void term_RIS(term_t *t);
void term_RIS_no_clear(term_t *t);

// A grid's rows, one at a time, as the view grows or shrinks. A grow or
// shrink with the cursor on the bottom row moves the screen, not the cursor.
void term_set_height(term_t *t, term_grid_t *g, uint8_t height);

// A grid's columns, as a program sets them: the cursors stay inside the
// rows, a pending wrap ends, the tab stops past the old width are the
// default ones, and the margins reset.
void term_set_width(term_t *t, term_grid_t *g, uint8_t width);

// Follow the terminal at one more grid, after the screen's: a copy of from,
// its cursors, margins, tab stops and line sizes, with its own line sizes in
// lines, rows for each screen. Dropping it leaves the others as they are.
void term_add_grid(term_t *t, term_grid_t *g, const term_grid_t *from,
                   uint8_t *lines, uint8_t rows);
void term_drop_grid(term_t *t, term_grid_t *g);

// The grid the queries are answered from: a program's own while it has one,
// otherwise the screen's showing grid (cells_shown).
term_grid_t *term_shown(term_t *t);

// The pen, the cursor and the line sizes of the screen showing.
static inline term_pen_t *term_pen(const term_t *t)
{
    return t->cur;
}
static inline term_pos_t *term_pos(const term_grid_t *g)
{
    return g->cur;
}
static inline uint8_t *term_lines(const term_grid_t *g)
{
    return g->ls;
}

// Whether the cells the pen writes are protected.
static inline bool term_protecting(const term_t *t)
{
    return t->cur->protect;
}

// Whether an erase leaves the protected cells alone: DECSED and DECSEL
// (dec), or ED, EL and ECH.
static inline bool term_erase_selective(const term_t *t, bool dec)
{
    if (dec)
        return t->protect_mode != TERM_PROTECT_OFF;
    return t->protect_mode == TERM_PROTECT_ISO && !t->erase_all;
}

// The columns of row y: half the width on a double-size line.
static inline unsigned term_cols(const term_grid_t *g, uint8_t y)
{
    return term_lines(g)[y] ? g->width / 2u : g->width;
}

// The last column a glyph or a move to the right reaches from the cursor:
// the right margin for a cursor at or left of it, otherwise the last column
// of its row.
static inline unsigned term_right_stop(const term_grid_t *g)
{
    const term_pos_t *pos = term_pos(g);
    unsigned last = term_cols(g, pos->y) - 1;
    return pos->x <= g->margin_right && g->margin_right < last ? g->margin_right : last;
}

// The first column a CR or a move to the left reaches: the left margin for
// a cursor at or right of it, otherwise the first column.
static inline unsigned term_left_stop(const term_grid_t *g)
{
    return term_pos(g)->x >= g->margin_left ? g->margin_left : 0;
}

// The pixel height of one character cell: 8x8 on the 320-wide canvases, where
// the 40-column terminal is drawn, 8x16 on the others.
static inline unsigned term_cell_height(const term_grid_t *g)
{
    return g->width == 40 ? 8 : 16;
}

/* ---- savestate ----------------------------------------------------------
 *
 * The rows of a terminal and of a grid for their owner's SST_TABLE (sst.h),
 * field by field, so no padding reaches the blob. The owner carries
 * alt_active, whose load is term_show, and the sizes a terminal is followed
 * at, which are its own. A grid's rows leave out its line sizes, which are
 * the owner's array. */

#define TERM_PEN_SST_ROWS(Y, p)                                        \
    Y(U8, p.sgr_attr)                                                  \
    Y(U8, p.fg.kind) Y(U8, p.fg.index) Y(U16, p.fg.rgb)                \
    Y(U8, p.fg.r) Y(U8, p.fg.g) Y(U8, p.fg.b)                          \
    Y(U8, p.bg.kind) Y(U8, p.bg.index) Y(U16, p.bg.rgb)                \
    Y(U8, p.bg.r) Y(U8, p.bg.g) Y(U8, p.bg.b)                          \
    Y(U8, p.ul.kind) Y(U8, p.ul.index) Y(U16, p.ul.rgb)                \
    Y(U8, p.ul.r) Y(U8, p.ul.g) Y(U8, p.ul.b)                          \
    Y(BOOL, p.ul_set) Y(BOOL, p.bold) Y(BOOL, p.faint)                 \
    Y(BOOL, p.reverse) Y(BOOL, p.conceal) Y(BOOL, p.protect)           \
    Y(BOOL, p.origin_mode) Y(U8, p.g0_charset) Y(U8, p.g1_charset)     \
    Y(BOOL, p.gl_is_g1)

#define TERM_OSC_ASK_SST_ROWS(Y, a) Y(U16, a.which) Y(U16, a.color)

#define TERM_SST_ROWS(X, t)                                            \
    X(STRUCT, t.pen[0], TERM_PEN_SST_ROWS)                             \
    X(STRUCT, t.saved[0], TERM_PEN_SST_ROWS)                           \
    X(BOOL, t.has_saved[0])                                            \
    X(STRUCT, t.pen[1], TERM_PEN_SST_ROWS)                             \
    X(STRUCT, t.saved[1], TERM_PEN_SST_ROWS)                           \
    X(BOOL, t.has_saved[1])                                            \
    X(BOOL, t.autowrap)                                                \
    X(BOOL, t.insert_mode)                                             \
    X(BOOL, t.newline_mode)                                            \
    X(BOOL, t.disp_ctrl)                                               \
    X(BOOL, t.vt52)                                                    \
    X(BOOL, t.app_cursor)                                              \
    X(BOOL, t.app_keypad)                                              \
    X(BOOL, t.autorepeat)                                              \
    X(BOOL, t.ice_colors)                                              \
    X(BOOL, t.bracketed_paste)                                         \
    X(U8, t.modify_other_keys)                                         \
    X(BOOL, t.format_other_keys)                                       \
    X(BOOL, t.lr_margins)                                              \
    X(BOOL, t.rect_attrs)                                              \
    X(U8, t.protect_mode)                                              \
    X(BOOL, t.erase_all)                                               \
    X(BOOL, t.reverse_screen)                                          \
    X(BOOL, t.smooth_scroll)                                           \
    X(BOOL, t.smooth_released)                                         \
    X(BOOL, t.cursor_enabled)                                          \
    X(U8, t.cursor_style)                                              \
    X(U8, t.last_glyph)                                                \
    X(BOOL, t.rep_valid)                                               \
    X(U8, t.ansi_state)                                                \
    X(U16S, t.csi_param)                                               \
    X(BYTES, t.csi_separator)                                          \
    X(U8, t.csi_param_count)                                           \
    X(U8, t.csi_private)                                               \
    X(U8, t.csi_intermediate)                                          \
    X(BOOL, t.csi_colon)                                               \
    X(BOOL, t.csi_any)                                                 \
    X(BOOL, t.osc_pending)                                             \
    X(BOOL, t.dcs_pending)                                             \
    X(U16, t.osc_ps)                                                   \
    X(U8, t.osc_stage)                                                 \
    X(U8, t.osc_item)                                                  \
    X(U16, t.osc_index)                                                \
    X(U8, t.osc_len)                                                   \
    X(BYTES, t.osc_spec)                                               \
    X(U8, t.osc_asks)                                                  \
    X(STRUCTS, t.osc_ask, TERM_OSC_ASK_SST_ROWS)

/* Whether what TERM_SST_ROWS loaded is in range: the parser's state and
 * modes, and the counts that index its arrays. False, after sst_refuse,
 * when not. */
bool term_sst_ok(sst_cursor_t *c, const term_t *t);

/* Shows the primary screen or the alternate: the pen, the cursors and the
 * line sizes follow. A savestate load calls it with the alt_active it
 * carries. */
void term_show(term_t *t, bool alt);

#define TERM_POS_SST_ROWS(Y, p)                                        \
    Y(U8, p.x) Y(U8, p.y) Y(BOOL, p.wrap_pending)

#define TERM_GRID_SST_ROWS(Y, g)                                       \
    Y(U8, g.width) Y(U8, g.height) Y(U8, g.rows)                       \
    Y(U8, g.margin_top) Y(U8, g.margin_bot)                            \
    Y(U8, g.margin_left) Y(U8, g.margin_right)                         \
    TERM_POS_SST_ROWS(Y, g.pos[0]) TERM_POS_SST_ROWS(Y, g.saved[0])    \
    TERM_POS_SST_ROWS(Y, g.pos[1]) TERM_POS_SST_ROWS(Y, g.saved[1])    \
    Y(BYTES, g.tab_stops)

/* Whether a grid TERM_GRID_SST_ROWS loaded, and its line sizes, fit
 * storage for rows rows, and everything that indexes the cells is in
 * range. False, after sst_refuse, when not. */
bool term_grid_sst_ok(sst_cursor_t *c, const term_grid_t *g, uint8_t rows);

/* A grid's rows and then its line sizes, for an owner whose grid is not
 * always followed and so needs an FN row of its own. A grid of other rows
 * than g has is refused. */
#define TERM_GRID_SST_SIZE(rows) (19 + TERM_TAB_BITMAP_BYTES + 2 * (rows))
void term_grid_sst_put(sst_cursor_t *c, const term_grid_t *g);
bool term_grid_sst_get(sst_cursor_t *c, term_grid_t *g);

#endif /* _CORE_TERM_TERM_H_ */
