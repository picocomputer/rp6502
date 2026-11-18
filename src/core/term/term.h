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
    bool cursor_enabled;  // DECTCEM, ?25
    uint8_t cursor_style; // DECSCUSR Ps: 0/1/2 block, 3/4 underline, 5/6 bar
    uint8_t last_glyph;   // for REP
    bool rep_valid;       // the last thing written was a glyph
    /* The parser. */
    ansi_state_t ansi_state;
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

// The pixel height of one character cell: 8x8 on the 320-wide canvases, where
// the 40-column terminal is drawn, 8x16 on the others.
static inline unsigned term_cell_height(const term_grid_t *g)
{
    return g->width == 40 ? 8 : 16;
}

#define TERM_PEN_SST_SIZE 32
#define TERM_SST_SIZE (4 * TERM_PEN_SST_SIZE + 108 + 4 * TERM_OSC_ASK_MAX)
void term_sst_put(sst_cursor_t *c, const term_t *t);
bool term_sst_get(sst_cursor_t *c, term_t *t);
#define TERM_GRID_SST_SIZE(rows) (19 + TERM_TAB_BITMAP_BYTES + 2 * (rows))
void term_grid_sst_put(sst_cursor_t *c, const term_grid_t *g);
bool term_grid_sst_get(sst_cursor_t *c, term_grid_t *g);

#endif /* _CORE_TERM_TERM_H_ */
