/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/term/color.h"
#include "core/term/term.h"
#include "core/com/manifold.h"
#include "core/vga/vga.h"
#include "core/vga/pixel_format.h"
#include "machine.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

// A VT102 with most of what xterm and the Linux console add: 256 colors and
// truecolor, the alternate screen, the dynamic colors of OSC 4/10/11/12, the
// Linux palette sequences and SGR 10/11. The parser follows the DEC/xterm
// state machine (vt100.net), so unknown sequences and strings are taken apart
// the way xterm does. Inside a sequence BS-SI and BEL act and the sequence
// goes on, as in xterm; any other control ends it silently, as in Linux. DEC Special
// Graphics (ESC ( 0 / SO / SI) is an attribute bit plus a parallel font buffer
// sourced from CP437. Bytes 0x80-0xFF are always glyphs of the code page:
// there are no C1 controls and no UTF-8.
//
// Designed to keep up with 115200 bps without flow control.
// The logic herein will make more sense if you remember this:

// 1. The screen data doesn't move when scrolling. Instead, the
//    video begins rendering at y_offset and wraps around.
// 2. The screen doesn't fully clear immediately. To keep the UART
//    buffer from overflowing, lines are cleared in a background task
//    and checked as the cursor moves into them. Nothing fills more
//    than one row of cells in a single operation.
// 3. Each cell's `attributes` byte holds render-time flags
//    (underline, blink, DEC graphics, etc.). The renderer's hot
//    path branches once on `attr != 0` per cell; the common path
//    pays only a 1-byte load and a predicted-not-taken branch.
// 4. The cursor's column never reaches the width. Writing the last
//    column sets wrap_pending instead, as on a VT102, and the next
//    glyph wraps. The view is told the column is the width while a
//    wrap is pending, which is how it knows to draw that state.

#define TERM_STD_HEIGHT 30
/* TERM_MAX_HEIGHT, the tallest terminal any view can ask for, is the
 * machine's: 32 where the 512-line SXGA console exists, 30 everywhere else. */
#define TERM_MAX_WIDTH 80
#define TERM_TAB_BITMAP_BYTES ((TERM_MAX_WIDTH + 7) / 8)
/* 16 parameters, as Linux's NPAR. More are dropped; the sequence still runs. */
#define TERM_CSI_PARAM_MAX_LEN 16
/* The longest answer term builds for a query. Replies snprintf into a buffer
 * this size and drop the reply if it would not fit, so this bounds what a
 * program can be told, not what the console can hold. */
#define TERM_REPLY_MAX 24
/* One OSC item, such as "rgb:ffff/ffff/ffff". Longer items are ignored. */
#define TERM_OSC_SPEC_MAX 20
#define TERM_FG_COLOR_INDEX 7
#define TERM_BG_COLOR_INDEX 0

// A blink is a thing on the screen, so it keeps the screen's time: these are
// frames, counted by vga_frame_count(), not microseconds. Every machine
// answers that tick, so a blink is the same blink on all of them -- and a
// period counted in frames cannot beat against the refresh it is drawn on,
// which is what the microsecond versions of these had to be detuned to avoid.

// Cell-blink base tick = the SGR 6 rapid-blink half-period. The phase counter
// (cell_blink_phase) increments every tick: bit 0 flips each tick (SGR 6 rapid,
// ~3 Hz), bit 1 every two ticks (SGR 5 slow, ~1.5 Hz -- the original rate).
// ~3 Hz clears ECMA-48's 150/min (2.5 Hz) slow/rapid boundary.
#define TERM_BLINK_TICK_FRAMES 10

// Cursor blink half-period: normal cell vs a pending wrap, which blinks at 2x
// rate so it's still noticeable.
#define TERM_CURSOR_BLINK_FRAMES 30
#define TERM_CURSOR_BLINK_FAST_FRAMES 15

// Whenever input changes the cursor's logical position or its
// appearance (DECSCUSR style, DECTCEM visibility), the cursor is
// forced unlit and its next blink tick is armed this far out. Pure
// SGR/OSC sequences and color-only changes (OSC 12/112) leave the
// blink schedule untouched. The gap re-lights to the appropriate
// steady/blink state when it fires -- one frame, which is as soon as
// anything can be seen.
#define TERM_CURSOR_INPUT_GAP_FRAMES 1

/* The C0 controls, as bitmaps of their codes. Linux acts on the ACTION set
 * and draws the rest as glyphs. With SGR 11 (or CRM, CSI 3 h) it draws
 * everything but the ALWAYS set. Inside a sequence the IN_SEQ set runs and
 * the sequence goes on, as in xterm. */
#define TERM_C0_ACTION 0x0D00FF81u
#define TERM_C0_ALWAYS 0x0800F501u
#define TERM_C0_IN_SEQ 0x0000FF80u
#define TERM_C0_CAN 0x18
#define TERM_C0_SUB 0x1A
#define TERM_C0_ESC 0x1B
#define TERM_DEL 0x7F

typedef enum
{
    ansi_state_ground,
    ansi_state_esc,         // after ESC
    ansi_state_esc_int,     // after ESC and an intermediate byte, 0x20-0x2F
    ansi_state_csi,         // after CSI, collecting parameters and intermediates
    ansi_state_csi_ignore,  // a malformed CSI, dropped through its final byte
    ansi_state_osc,         // an OSC string, up to BEL or ST
    ansi_state_osc_palette, // Linux ESC ] P nrrggbb, seven hex digits
    ansi_state_string,      // DCS, SOS, PM or APC, ignored up to ST
    ansi_state_vt52_esc,    // after ESC in VT52 mode
    ansi_state_vt52_row,    // after ESC Y
    ansi_state_vt52_col,    // after ESC Y and the row
    ansi_state_count
} ansi_state_t;

// The character sets that G0 and G1 can hold.
#define TERM_CS_ASCII 0 // ESC ( B, and the null and user maps ESC ( U / K
#define TERM_CS_DEC 1   // ESC ( 0, DEC Special Graphics
#define TERM_CS_UK 2    // ESC ( A, ASCII with the pound sign at 0x23

// How the pen's foreground or background color was chosen. A palette color
// keeps its index so OSC 4, 104 and the Linux palette can refresh it, and the
// default colors are their own kind, apart from palette 7 and 0, so SGR 39/49
// are not SGR 37/40 and OSC 4 never changes the defaults.
#define TERM_COLOR_DEFAULT 0
#define TERM_COLOR_INDEX 1
#define TERM_COLOR_RGB 2

// The cursor and its pen: position, SGR and the character sets. It is the
// active state of a screen, and also the type of the DECSC slot, so the field
// list is defined once.
typedef struct
{
    uint8_t x, y;
    bool wrap_pending; // the last column was written; the next glyph wraps
    uint8_t sgr_attr;
    uint8_t fg_kind, fg_index;
    uint8_t bg_kind, bg_index;
    uint16_t fg_rgb, bg_rgb;  // the base color when the kind is TERM_COLOR_RGB
    uint16_t fg_color;        // resolved: bold, faint and the palette applied
    uint16_t bg_color;        // resolved: iCE colors and the palette applied
    uint16_t ul_color;        // SGR 58, valid only when underline_color_set
    bool underline_color_set; // SGR 58 active; SGR 59 / SGR 0 clear it
    bool bold;
    bool faint;
    bool reverse; // SGR 7: emit-time fg/bg swap
    bool conceal; // SGR 8: emit-time fg = bg
    bool origin_mode;
    uint8_t g0_charset, g1_charset;
    bool gl_is_g1;
} cursor_state_t;

// Per-screen-buffer state. The terminal carries two of these (primary and
// alternate) and a pointer to the currently active one. Swapping screens is
// a single pointer write -- cursor + SGR follow automatically via the cs
// field.
typedef struct
{
    term_data_t *mem;
    cursor_state_t cs;
    cursor_state_t saved; // DECSC, CSI s, ?1048 and ?1049
    bool has_saved;       // a DECSC since the last reset, for DECRQM ?1048
    uint8_t row_idx[TERM_MAX_HEIGHT];
    bool dirty[TERM_MAX_HEIGHT];
    uint16_t erase_fg_color[TERM_MAX_HEIGHT];
    uint16_t erase_bg_color[TERM_MAX_HEIGHT];
    uint8_t y_offset;
    /* The rows scrolled off the top that are still in the cell store, the
     * newest first. A taller view shows them again; ED 3 forgets them. */
    uint8_t hidden;
    bool all_clean;
} screen_buf_t;

typedef struct term_state
{
    uint8_t width;
    uint8_t height;
    /* Per-screen state. cur is &screen->cs, swapped with screen on alt
     * enter/exit. All cursor + SGR access goes through cur. */
    screen_buf_t bufs[2]; /* [0] = primary, [1] = alt */
    screen_buf_t *screen; /* active buffer */
    cursor_state_t *cur;  /* = &screen->cs */
    bool alt_active;      /* true when screen == &bufs[1] */
    /* The scroll margins belong to the terminal and both screens share them,
     * as in xterm. */
    uint8_t margin_top, margin_bot;
    bool autowrap;     // DECAWM, ?7
    bool insert_mode;  // IRM, CSI 4 h
    bool newline_mode; // LNM, CSI 20 h: LF, VT and FF also return the carriage
    bool disp_ctrl;    // SGR 11 or CRM (CSI 3 h): draw the C0 controls
    bool vt52;         // DECANM reset, ?2 l
    bool ice_colors;   // ?33 -- SGR 5/6 means bright bg (the IBM VGA hack), not blink
    bool cursor_enabled;
    uint8_t cursor_style; // DECSCUSR Ps: 0/1/2=block, 3/4=underline, 5/6=bar (read by the view)
    unsigned long cursor_frame;
    volatile bool cursor_lit;
    unsigned long cell_blink_frame;
    volatile uint8_t cell_blink_phase;
    uint16_t default_fg_color;
    uint16_t default_bg_color;
    uint16_t cursor_color; // OSC 12
    uint8_t last_glyph;    // for REP
    bool rep_valid;        // the last thing written was a glyph
    uint8_t tab_stops[TERM_TAB_BITMAP_BYTES]; // 1 bit per column
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
    uint16_t osc_ps;          // the OSC command number
    uint8_t osc_stage;        // 0 collecting the number, 1 the items, 2 dropping the string
    uint8_t osc_item;         // how many items of the command are done
    uint16_t osc_index;       // OSC 4: the palette entry the next color is for
    uint8_t osc_len;          // bytes in osc_spec, or more than TERM_OSC_SPEC_MAX when too long
    char osc_spec[TERM_OSC_SPEC_MAX];
} term_state_t;

static term_state_t term_40;
static term_state_t term_80;

// Add `row` to `y_offset` and wrap into [0, TERM_MAX_HEIGHT). Used to
// translate a logical row into a physical row_idx[] slot. Inputs are
// constrained to [0, TERM_MAX_HEIGHT), so the sum can't exceed 2*MAX-1
// (=63), which fits in uint8_t.
static inline uint8_t term_buf_slot(uint8_t y_offset, uint8_t row)
{
    uint8_t slot = (uint8_t)(y_offset + row);
    if (slot >= TERM_MAX_HEIGHT)
        slot -= TERM_MAX_HEIGHT;
    return slot;
}

// Translate a logical row (0..height-1) into the start of its physical
// cell row. Reads y_offset and row_idx[] without locking; the renderer
// on Core 1 uses the same path. Worst case is one frame of visual tear
// while a region scroll is mid-update. No memory barrier required.
static inline term_data_t *term_row_ptr(const term_state_t *term, uint8_t y)
{
    uint8_t slot = term_buf_slot(term->screen->y_offset, y);
    return term->screen->mem + (uint32_t)term->screen->row_idx[slot] * term->width;
}

static inline uint16_t term_param(const term_state_t *term, uint8_t i, uint16_t dflt)
{
    uint16_t v = (i < term->csi_param_count) ? term->csi_param[i] : 0;
    return v ? v : dflt;
}

/* ---- colors ------------------------------------------------------------ */

static uint16_t term_faint(uint16_t c)
{
    uint16_t r = SCANVIDEO_R5_FROM_PIXEL(c) >> 1;
    uint16_t g = SCANVIDEO_G5_FROM_PIXEL(c) >> 1;
    uint16_t b = SCANVIDEO_B5_FROM_PIXEL(c) >> 1;
    return SCANVIDEO_PIXEL_FROM_RGB5(r, g, b) | (c & SCANVIDEO_ALPHA_MASK);
}

// The pen's foreground before bold and faint: what SGR 38;1 makes transparent.
static uint16_t term_fg_base(const term_state_t *term, const cursor_state_t *cs)
{
    if (cs->fg_kind == TERM_COLOR_INDEX)
        return color_256_term[cs->fg_index];
    if (cs->fg_kind == TERM_COLOR_RGB)
        return cs->fg_rgb;
    return term->default_fg_color;
}

static uint16_t term_bg_base(const term_state_t *term, const cursor_state_t *cs)
{
    if (cs->bg_kind == TERM_COLOR_INDEX)
        return color_256_term[cs->bg_index];
    if (cs->bg_kind == TERM_COLOR_RGB)
        return cs->bg_rgb;
    return term->default_bg_color;
}

// Resolve the pen's colors. Bold brightens palette colors 0-7 and the
// default foreground (to 15, as Linux does); faint halves each channel. In
// iCE colors mode (?33) SGR 5/6 brightens a background of 0-7 or the default
// instead of blinking -- the IBM-VGA hack the BBS scene relies on.
static void term_resolve_pen(const term_state_t *term, cursor_state_t *cs)
{
    uint16_t fg;
    if (cs->bold && cs->fg_kind == TERM_COLOR_INDEX && cs->fg_index < 8)
        fg = color_256_term[cs->fg_index + 8];
    else if (cs->bold && cs->fg_kind == TERM_COLOR_DEFAULT)
        fg = color_256_term[15];
    else
        fg = term_fg_base(term, cs);
    cs->fg_color = cs->faint ? term_faint(fg) : fg;

    bool bright = term->ice_colors && (cs->sgr_attr & TERM_ATTR_ANY_BLINK);
    if (bright && cs->bg_kind == TERM_COLOR_INDEX && cs->bg_index < 8)
        cs->bg_color = color_256_term[cs->bg_index + 8];
    else if (bright && cs->bg_kind == TERM_COLOR_DEFAULT)
        cs->bg_color = color_256_term[8];
    else
        cs->bg_color = term_bg_base(term, cs);
}

// After the palette, the default colors or iCE colors change, every pen the
// terminal holds is resolved again: both screens' and both saved cursors.
static void term_resolve_all_pens(term_state_t *term)
{
    for (int b = 0; b < 2; b++)
    {
        term_resolve_pen(term, &term->bufs[b].cs);
        term_resolve_pen(term, &term->bufs[b].saved);
    }
}

// Compute the effective fg/bg/attr a cell write should land with, given
// the current SGR state. Applies emit-time REVERSE/CONCEAL toggles; render
// bits, meaning everything in TERM_ATTR_RENDER_MASK, flow through unchanged.
// TERM_ATTR_DEC is the caller's responsibility -- only set for DEC glyph cells.
// In ice_colors mode (DECSET ?33) both blink bits (TERM_ATTR_ANY_BLINK) are
// suppressed at the cell level -- the bright bg is already baked into
// bg_color and we don't want the renderer to also pulse the cell.
static inline void term_emit_attrs(const term_state_t *term,
                                   uint16_t *fg, uint16_t *bg, uint8_t *attr)
{
    uint16_t f = term->cur->fg_color;
    uint16_t b = term->cur->bg_color;
    if (term->cur->reverse)
    {
        uint16_t t = f;
        f = b;
        b = t;
    }
    if (term->cur->conceal)
        f = b;
    *fg = f;
    *bg = b;
    uint8_t cell_mask = TERM_ATTR_RENDER_MASK & ~TERM_ATTR_DEC;
    if (term->ice_colors)
        cell_mask &= (uint8_t)~TERM_ATTR_ANY_BLINK;
    *attr = (uint8_t)(term->cur->sgr_attr & cell_mask);
}

/* ---- cells and rows ---------------------------------------------------- */

// Paint [start, end) of `row` with erased cells: a space in the given colors
// with no attributes. An erase takes the pen's colors, not reversed, and
// none of its attributes, as xterm and Linux do. The foreground matters too,
// because the cursor is drawn from the cell it sits on.
static inline void fill_cells(term_data_t *row, unsigned start, unsigned end,
                              uint16_t fg, uint16_t bg)
{
    for (unsigned i = start; i < end; i++)
    {
        row[i].font_code = ' ';
        row[i].attributes = 0;
        row[i].fg_color = fg;
        row[i].bg_color = bg;
        row[i].ul_color = fg;
    }
}

// Make sure you call this any time you change rows.
// It will process any pending screen clears on the row.
static void term_clean_line(term_state_t *term, uint8_t y)
{
    if (!term->screen->dirty[y])
        return;
    term->screen->dirty[y] = false;
    fill_cells(term_row_ptr(term, y), 0, term->width,
               term->screen->erase_fg_color[y],
               term->screen->erase_bg_color[y]);
}

// Force the cursor unlit and arm the input-gap timer. Used by movement
// (term_out_chars) and by appearance changes (DECSCUSR, DECTCEM enable)
// so the cursor's new state lands on a clean blink boundary.
static inline void term_cursor_restart_blink(term_state_t *term)
{
    term->cursor_lit = false;
    term->cursor_frame = vga_frame_count() + TERM_CURSOR_INPUT_GAP_FRAMES;
}

// Restart the cursor blink and the blinking text together, both lit, so a
// reset leaves them in step.
static void term_restart_blinks(term_state_t *term)
{
    term_cursor_restart_blink(term);
    term->cell_blink_phase = 0;
    term->cell_blink_frame = vga_frame_count() + TERM_BLINK_TICK_FRAMES;
}

// Move the cursor, 0-indexed, clamped to the screen. Every move ends a
// pending wrap, and the destination row is cleaned so a glyph can land on it.
static void term_goto(term_state_t *term, unsigned x, unsigned y)
{
    if (x >= term->width)
        x = term->width - 1u;
    if (y >= term->height)
        y = term->height - 1u;
    term->cur->x = (uint8_t)x;
    term->cur->y = (uint8_t)y;
    term->cur->wrap_pending = false;
    term_clean_line(term, (uint8_t)y);
}

// Clean one dirty row in `buf`. Returns true if a row was cleaned this
// call, false if the buffer was already fully clean. Operates on an
// explicit buffer so the task path can drain the inactive (alt) buffer
// too -- otherwise the lazy mark on RIS would leave alt dirty until ?47
// swaps it in.
static bool term_clean_one_dirty(screen_buf_t *buf, uint8_t width, uint8_t height)
{
    if (buf->all_clean)
        return false;
    for (size_t i = 0; i < height; i++)
    {
        if (buf->dirty[i])
        {
            buf->dirty[i] = false;
            uint8_t slot = term_buf_slot(buf->y_offset, (uint8_t)i);
            term_data_t *row = buf->mem + (uint32_t)buf->row_idx[slot] * width;
            fill_cells(row, 0, width, buf->erase_fg_color[i], buf->erase_bg_color[i]);
            return true;
        }
    }
    buf->all_clean = true;
    return false;
}

// Clean at most one row per tick: prefer the active buffer; only touch
// the inactive one if the active is already clean. Invariant: cur->y is
// never dirty on the active buffer (every code path that marks a row
// dirty either avoids cur->y or follows up with term_clean_line on it),
// so the cursor cell is never affected by this task and it never has to
// call term_cursor_restart_blink.
static void term_clean_task(term_state_t *term)
{
    if (term_clean_one_dirty(term->screen, term->width, term->height))
        return;
    screen_buf_t *other = &term->bufs[term->alt_active ? 0 : 1];
    if (other->mem)
        term_clean_one_dirty(other, term->width, term->height);
}

static void term_shift_meta_up(screen_buf_t *s, uint8_t count)
{
    for (uint8_t i = 0; i < count; i++)
    {
        s->dirty[i] = s->dirty[i + 1];
        s->erase_fg_color[i] = s->erase_fg_color[i + 1];
        s->erase_bg_color[i] = s->erase_bg_color[i + 1];
    }
}

static void term_shift_meta_down(screen_buf_t *s, uint8_t start)
{
    for (int i = start; i > 0; i--)
    {
        s->dirty[i] = s->dirty[i - 1];
        s->erase_fg_color[i] = s->erase_fg_color[i - 1];
        s->erase_bg_color[i] = s->erase_bg_color[i - 1];
    }
}

static void term_mark_rows(screen_buf_t *s, uint8_t from, uint8_t to,
                           uint16_t fg, uint16_t bg)
{
    for (uint8_t i = from; i < to; i++)
    {
        s->dirty[i] = true;
        s->erase_fg_color[i] = fg;
        s->erase_bg_color[i] = bg;
    }
    s->all_clean = false;
}

// Lazy-erase rows [from, to) of the active screen in the pen's colors.
static void term_mark_rows_erase(term_state_t *term, uint8_t from, uint8_t to)
{
    term_mark_rows(term->screen, from, to, term->cur->fg_color, term->cur->bg_color);
}

// Lazy clear of the given screen buffer: resets y_offset / row_idx to
// identity and marks every visible row dirty with the given erase colors.
// No cell memory is touched here -- term_clean_task drains dirty rows in
// the background (one row per tick, on both buffers), and the active
// buffer's cursor row is also cleaned synchronously when a write lands
// via term_goto -> term_clean_line. Used by RIS for both buffers and by the
// alt-screen clears of ?1047 and ?1049.
static void term_mark_buf_erased(screen_buf_t *buf, uint8_t height,
                                 uint16_t fg, uint16_t bg)
{
    buf->y_offset = 0;
    buf->hidden = 0;
    for (uint8_t y = 0; y < TERM_MAX_HEIGHT; y++)
    {
        buf->row_idx[y] = y;
        buf->dirty[y] = false;
    }
    term_mark_rows(buf, 0, height, fg, bg);
}

static void term_reset_tab_stops(term_state_t *term)
{
    memset(term->tab_stops, 0, sizeof(term->tab_stops));
    for (uint8_t col = 8; col < term->width; col += 8)
        term->tab_stops[col >> 3] |= 1u << (col & 7);
}

/* ---- resets ------------------------------------------------------------ */

// The power-up pen: SGR 0, home, no pending wrap, ASCII in G0 and G1, origin
// mode off. It is also what DECRC restores when nothing was saved.
static void term_default_cursor(const term_state_t *term, cursor_state_t *cs)
{
    memset(cs, 0, sizeof *cs);
    term_resolve_pen(term, cs);
}

// Reset the pen's SGR, origin mode and character sets, leaving its position.
static void term_reset_pen(const term_state_t *term, cursor_state_t *cs)
{
    uint8_t x = cs->x, y = cs->y;
    term_default_cursor(term, cs);
    cs->x = x;
    cs->y = y;
}

static void term_reset_parser(term_state_t *term)
{
    term->ansi_state = ansi_state_ground;
    term->csi_intermediate = 0;
    term->osc_pending = false;
    term->rep_valid = false;
}

// Everything RIS and the system's soft reset share: every mode and setting,
// the palette and the default and cursor colors, the tab stops, the margins,
// both pens and both saved cursors, and the blinks restarted in step. The
// alternate screen is dropped and erased. Only the size, the code page, the
// primary screen's cells and the cursor's position are left. New state added
// later joins this list.
static void term_reset_core(term_state_t *term)
{
    /* The runtime palette is shared by both terminals, so this runs twice;
     * the second time does nothing new. */
    memcpy(color_256_term, color_256, sizeof(color_256_term));
    term->screen = &term->bufs[0];
    term->cur = &term->screen->cs;
    term->alt_active = false;
    term->default_fg_color = color_256[TERM_FG_COLOR_INDEX];
    term->default_bg_color = color_256[TERM_BG_COLOR_INDEX];
    term->cursor_color = color_256[TERM_FG_COLOR_INDEX];
    term->margin_top = 0;
    term->margin_bot = (uint8_t)(term->height - 1);
    term->autowrap = true;
    term->insert_mode = false;
    term->newline_mode = false;
    term->disp_ctrl = false;
    term->vt52 = false;
    term->ice_colors = false;
    term->cursor_enabled = true;
    term->cursor_style = 0;
    term_reset_parser(term);
    term_reset_tab_stops(term);
    for (int b = 0; b < 2; b++)
    {
        term_reset_pen(term, &term->bufs[b].cs);
        term_default_cursor(term, &term->bufs[b].saved);
        term->bufs[b].has_saved = false;
    }
#if TERM_ALT_SCREEN
    term_mark_buf_erased(&term->bufs[1], term->height,
                         term->default_fg_color, term->default_bg_color);
#endif
    term_restart_blinks(term);
}

// RIS, ESC c: a new terminal in one predictable state, whatever came before.
static void term_out_RIS(term_state_t *term)
{
    term_reset_core(term);
    term_mark_buf_erased(term->screen, term->height,
                         term->default_fg_color, term->default_bg_color);
    term_goto(term, 0, 0);
}

// The system's soft reset, when a program ends and at a debug boot: what RIS
// does but leaving the screen as it is and the cursor where it is. Out-of-band
// (by the Pico VGA's DISPLAY xreg or vga_console_reset, not term_out_chars).
static void term_out_RIS_no_clear(term_state_t *term)
{
    term_reset_core(term);
    term_goto(term, term->cur->x, term->cur->y);
}

// DECSTR, CSI ! p: the soft reset a program sends. It resets the modes on
// DEC's list -- IRM, DECOM, DECAWM (to our default, on), DECTCEM, the
// character sets, SGR, the margins and the saved cursor -- and ends a pending
// wrap, but leaves the cursor where it is. The cursor style, iCE colors, the
// palette, the tab stops and the screen are left alone.
static void term_out_DECSTR(term_state_t *term)
{
    term->insert_mode = false;
    term->autowrap = true;
    term->cursor_enabled = true;
    term->margin_top = 0;
    term->margin_bot = (uint8_t)(term->height - 1);
    term_reset_pen(term, term->cur);
    term_default_cursor(term, &term->bufs[0].saved);
    term_default_cursor(term, &term->bufs[1].saved);
    term->bufs[0].has_saved = false;
    term->bufs[1].has_saved = false;
    term_goto(term, term->cur->x, term->cur->y);
}

static void term_state_init(term_state_t *term, uint8_t width,
                            term_data_t *mem_pri, term_data_t *mem_alt)
{
    term->width = width;
    term->height = TERM_STD_HEIGHT;
    term->bufs[0].mem = mem_pri;
    term->bufs[1].mem = mem_alt;
    term->cursor_lit = false;
    term_out_RIS(term);
}

/* ---- height ------------------------------------------------------------ */

static void term_state_set_height(term_state_t *term, uint8_t height)
{
    assert(height >= 1 && height <= TERM_MAX_HEIGHT);
    screen_buf_t *s = term->screen;
    while (height != term->height)
    {
        if (height > term->height)
        {
            term->height++;
            if (term->cur->y == term->height - 2)
            {
                // The cursor is on the bottom row, so the view grows at the
                // top: everything moves down a row and the row above comes
                // into view. It is a row of scrollback when there is one,
                // otherwise a blank one.
                term->cur->y++;
                if (!s->y_offset)
                    s->y_offset = TERM_MAX_HEIGHT - 1;
                else
                    s->y_offset--;
                term_shift_meta_down(s, (uint8_t)(term->height - 1));
                if (s->hidden)
                {
                    s->hidden--;
                    s->dirty[0] = false;
                }
                else
                    term_mark_rows(s, 0, 1, term->default_fg_color, term->default_bg_color);
                // shift_meta_down may have copied a dirty=true flag into
                // the cursor's new row; uphold the "cur->y is never
                // dirty on the active buffer" invariant.
                term_clean_line(term, term->cur->y);
            }
            else
            {
                // Expose one fresh row at the bottom: mark it dirty with the
                // default colors. The cleaner task drains it in the
                // background; the worst case is one row of stale content for
                // a frame or two.
                uint8_t y = (uint8_t)(term->height - 1);
                term_mark_rows(s, y, (uint8_t)(y + 1),
                               term->default_fg_color, term->default_bg_color);
            }
        }
        else
        {
            term->height--;
            if (term->cur->y == term->height)
            {
                // The cursor is on the bottom row, so the top row goes out of
                // view and joins the scrollback.
                term->cur->y--;
                if (++s->y_offset >= TERM_MAX_HEIGHT)
                    s->y_offset -= TERM_MAX_HEIGHT;
                term_shift_meta_up(s, term->height);
                s->hidden++;
                term_clean_line(term, term->cur->y);
            }
            // Otherwise the bottom row is no longer visible; the cleaner only
            // drains visible rows. Leave it -- a later grow that re-exposes
            // the row will mark it dirty via the branch above.
        }
        // The rows above the view and the rows below it share the ring, so
        // the scrollback is at most what the view leaves over.
        uint8_t room = (uint8_t)(TERM_MAX_HEIGHT - term->height);
        for (int b = 0; b < 2; b++)
            if (term->bufs[b].hidden > room)
                term->bufs[b].hidden = room;
    }
    term->margin_top = 0;
    term->margin_bot = (uint8_t)(term->height - 1);

    // The resize loop only tracks the active buffer's cursor. Any other
    // cursor row (the inactive buffer's cs and both saved cursors) can exceed
    // the new height after a shrink; clamp them so a later swap or DECRC
    // doesn't strand the cursor below the viewport.
    uint8_t max_y = (uint8_t)(term->height - 1);
    screen_buf_t *inactive = &term->bufs[term->alt_active ? 0 : 1];
    if (inactive->cs.y > max_y)
        inactive->cs.y = max_y;
    for (int b = 0; b < 2; b++)
        if (term->bufs[b].saved.y > max_y)
            term->bufs[b].saved.y = max_y;
}

/* ---- scrolling --------------------------------------------------------- */

// Scroll the inclusive region [top, bot] up by n rows in a single pass.
// Content moves up; the n newly-exposed rows at the bottom are marked
// dirty with the pen's erase colors. Cell writes are deferred to
// term_clean_task. Caller is responsible for cursor positioning and for
// cleaning the cursor's row.
static void term_region_scroll_up(term_state_t *term, uint8_t top,
                                  uint8_t bot, uint16_t count)
{
    if (top > bot || count == 0)
        return;
    uint8_t region_h = (uint8_t)(bot - top + 1);
    uint8_t n = count > region_h ? region_h : (uint8_t)count;
    screen_buf_t *s = term->screen;

    // Rotate row_idx within the region by n: save the top n slots, shift
    // the rest down by n, place the saved n at the bottom of the region.
    uint8_t y_off = s->y_offset;
    uint8_t saved[TERM_MAX_HEIGHT];
    for (uint8_t i = 0; i < n; i++)
        saved[i] = s->row_idx[term_buf_slot(y_off, (uint8_t)(top + i))];
    for (uint8_t i = top; (uint8_t)(i + n) <= bot; i++)
        s->row_idx[term_buf_slot(y_off, i)] = s->row_idx[term_buf_slot(y_off, (uint8_t)(i + n))];
    for (uint8_t i = 0; i < n; i++)
        s->row_idx[term_buf_slot(y_off, (uint8_t)(bot - n + 1 + i))] = saved[i];

    // Content moved up by n, so each row inherits the metadata of the
    // row n positions below it.
    for (uint8_t i = top; (uint8_t)(i + n) <= bot; i++)
    {
        s->dirty[i] = s->dirty[i + n];
        s->erase_fg_color[i] = s->erase_fg_color[i + n];
        s->erase_bg_color[i] = s->erase_bg_color[i + n];
    }
    term_mark_rows_erase(term, (uint8_t)(bot - n + 1), (uint8_t)(bot + 1));
}

// Symmetric to term_region_scroll_up: shifts content down by n rows; the
// n newly-exposed rows at the top are marked dirty (lazy clear).
static void term_region_scroll_down(term_state_t *term, uint8_t top,
                                    uint8_t bot, uint16_t count)
{
    if (top > bot || count == 0)
        return;
    uint8_t region_h = (uint8_t)(bot - top + 1);
    uint8_t n = count > region_h ? region_h : (uint8_t)count;
    screen_buf_t *s = term->screen;

    uint8_t y_off = s->y_offset;
    uint8_t saved[TERM_MAX_HEIGHT];
    for (uint8_t i = 0; i < n; i++)
        saved[i] = s->row_idx[term_buf_slot(y_off, (uint8_t)(bot - n + 1 + i))];
    for (int i = (int)bot; i - (int)n >= (int)top; i--)
        s->row_idx[term_buf_slot(y_off, (uint8_t)i)] = s->row_idx[term_buf_slot(y_off, (uint8_t)(i - n))];
    for (uint8_t i = 0; i < n; i++)
        s->row_idx[term_buf_slot(y_off, (uint8_t)(top + i))] = saved[i];

    for (int i = (int)bot; i - (int)n >= (int)top; i--)
    {
        s->dirty[i] = s->dirty[i - n];
        s->erase_fg_color[i] = s->erase_fg_color[i - n];
        s->erase_bg_color[i] = s->erase_bg_color[i - n];
    }
    term_mark_rows_erase(term, top, (uint8_t)(top + n));
}

// Index: down a row, scrolling the region at its bottom margin. The core of
// LF, VT, FF, IND and NEL. Below the region the cursor walks down to the last
// row and never scrolls.
static void term_index(term_state_t *term)
{
    cursor_state_t *cur = term->cur;
    cur->wrap_pending = false;
    if (cur->y != term->margin_bot)
    {
        if (cur->y < term->height - 1)
            ++cur->y;
    }
    else if (term->margin_top == 0 && term->margin_bot == term->height - 1)
    {
        // Fast path: rotate the viewport via y_offset, mark the new bottom
        // row dirty. The row that scrolled off joins the scrollback.
        screen_buf_t *s = term->screen;
        if (++s->y_offset == TERM_MAX_HEIGHT)
            s->y_offset = 0;
        if (s->hidden < TERM_MAX_HEIGHT - term->height)
            s->hidden++;
        term_shift_meta_up(s, (uint8_t)(term->height - 1));
        term_mark_rows_erase(term, (uint8_t)(term->height - 1), term->height);
    }
    else
        term_region_scroll_up(term, term->margin_top, term->margin_bot, 1);
    term_clean_line(term, cur->y);
}

// Reverse Index (ESC M) — dual of IND. At margin_top it scrolls the region
// (or whole screen) down by one row; elsewhere the cursor moves up, stopping
// at the top row.
static void term_reverse_index(term_state_t *term)
{
    cursor_state_t *cur = term->cur;
    cur->wrap_pending = false;
    if (cur->y != term->margin_top)
    {
        if (cur->y > 0)
            --cur->y;
    }
    else if (term->margin_top == 0 && term->margin_bot == term->height - 1)
    {
        // The row coming in at the top is the newest scrollback slot, which
        // is lost.
        screen_buf_t *s = term->screen;
        s->y_offset = s->y_offset ? (uint8_t)(s->y_offset - 1) : (uint8_t)(TERM_MAX_HEIGHT - 1);
        if (s->hidden)
            s->hidden--;
        term_shift_meta_down(s, (uint8_t)(term->height - 1));
        term_mark_rows_erase(term, 0, 1);
    }
    else
        term_region_scroll_down(term, term->margin_top, term->margin_bot, 1);
    term_clean_line(term, cur->y);
}

static void term_out_CR(term_state_t *term)
{
    term->cur->x = 0;
    term->cur->wrap_pending = false;
}

// LF, VT and FF: an index, and a carriage return too under LNM.
static void term_out_LF(term_state_t *term)
{
    term_index(term);
    if (term->newline_mode)
        term_out_CR(term);
}

/* ---- glyphs ------------------------------------------------------------ */

// The cell a glyph lands as: the pen's colors and attributes, through the
// active character set. DEC Special Graphics is applied at render time, not
// at the font_code level: the cell keeps the original byte and is flagged so
// the renderer pulls from font_dec_*. The UK set's pound sign is the DEC set's
// 0x7D. The 0x80..0xFF font range stays reserved for the active codepage.
static term_data_t term_glyph_cell(const term_state_t *term, uint8_t byte)
{
    const cursor_state_t *cur = term->cur;
    term_data_t cell;
    term_emit_attrs(term, &cell.fg_color, &cell.bg_color, &cell.attributes);
    uint8_t set = cur->gl_is_g1 ? cur->g1_charset : cur->g0_charset;
    if (set == TERM_CS_DEC && byte >= 0x5F && byte <= 0x7E)
        cell.attributes |= TERM_ATTR_DEC;
    else if (set == TERM_CS_UK && byte == '#')
    {
        byte = 0x7D;
        cell.attributes |= TERM_ATTR_DEC;
    }
    cell.font_code = byte;
    cell.ul_color = cur->underline_color_set ? cur->ul_color : cell.fg_color;
    return cell;
}

// A glyph is about to be written: a pending wrap goes to the next row first,
// with DECAWM on.
static void term_wrap(term_state_t *term)
{
    cursor_state_t *cur = term->cur;
    if (!cur->wrap_pending)
        return;
    cur->wrap_pending = false;
    if (term->autowrap)
    {
        cur->x = 0;
        term_index(term);
    }
}

// Write one glyph at the cursor and move on. Writing the last column leaves
// the cursor there with a wrap pending (DECAWM on), or just stays there
// (DECAWM off). In insert mode (IRM) the rest of the row moves right first,
// and what passes the right edge is lost.
static void term_put_glyph(term_state_t *term, uint8_t byte)
{
    cursor_state_t *cur = term->cur;
    term_data_t cell = term_glyph_cell(term, byte);
    term_wrap(term);
    term_data_t *row = term_row_ptr(term, cur->y);
    if (term->insert_mode)
        memmove(&row[cur->x + 1], &row[cur->x],
                (size_t)(term->width - 1 - cur->x) * sizeof(term_data_t));
    row[cur->x] = cell;
    if (cur->x < term->width - 1)
        cur->x++;
    else if (term->autowrap)
        cur->wrap_pending = true;
}

// A graphic character from the stream, which REP can repeat.
static void term_print(term_state_t *term, uint8_t byte)
{
    term->last_glyph = byte;
    term->rep_valid = true;
    term_put_glyph(term, byte);
}

// REP, CSI Pn b: the last graphic character again, Pn times (xterm,
// ECMA-48). Nothing when a control came after it. After a row more than the
// cell store holds, every row the glyphs can reach is full of the glyph, the
// scrollback included, and each further row of them changes nothing; so a
// long count runs only that far, plus its remainder of a row. The glyphs go
// a row at a time, so a long REP costs about what printing the rows does.
static void term_out_REP(term_state_t *term)
{
    if (!term->rep_valid)
        return;
    unsigned n = term_param(term, 0, 1);
    unsigned most = term->width * (TERM_MAX_HEIGHT + 1u);
    if (n > most)
        n = most + (n - most) % term->width;
    if (term->insert_mode)
    {
        while (n--)
            term_put_glyph(term, term->last_glyph);
        return;
    }
    cursor_state_t *cur = term->cur;
    term_data_t cell = term_glyph_cell(term, term->last_glyph);
    while (n)
    {
        term_wrap(term);
        unsigned run = (unsigned)(term->width - cur->x);
        if (run > n)
            run = n;
        term_data_t *row = term_row_ptr(term, cur->y) + cur->x;
        for (unsigned i = 0; i < run; i++)
            row[i] = cell;
        n -= run;
        if (cur->x + run < term->width)
            cur->x = (uint8_t)(cur->x + run);
        else
        {
            // The last column: what is left wraps, or with DECAWM off lands
            // on that column again, which already holds the glyph.
            cur->x = (uint8_t)(term->width - 1);
            cur->wrap_pending = term->autowrap;
            if (!term->autowrap)
                n = 0;
        }
    }
}

/* ---- tabs -------------------------------------------------------------- */

static inline bool term_tab_is_set(const term_state_t *term, uint8_t col)
{
    return (term->tab_stops[col >> 3] & (1u << (col & 7))) != 0;
}

// Forward to the next tab stop, or the last column when there is none
// (Linux and xterm: HT stops at the edge, it doesn't wrap). A pending wrap
// stays pending, as in Linux and xterm: the cursor is already at the edge.
static void term_tab_forward(term_state_t *term, uint16_t n)
{
    cursor_state_t *cur = term->cur;
    while (n-- && cur->x < term->width - 1)
    {
        do
            cur->x++;
        while (cur->x < term->width - 1 && !term_tab_is_set(term, cur->x));
    }
}

// Back to the previous tab stop, or the first column when there is none.
static void term_tab_back(term_state_t *term, uint16_t n)
{
    cursor_state_t *cur = term->cur;
    cur->wrap_pending = false;
    while (n-- && cur->x > 0)
    {
        do
            cur->x--;
        while (cur->x > 0 && !term_tab_is_set(term, cur->x));
    }
}

// Tab Clear. Ps=0 (default) clears the stop at the cursor; Ps=3 clears all.
static void term_out_TBC(term_state_t *term)
{
    uint16_t ps = term->csi_param[0];
    if (ps == 3)
        memset(term->tab_stops, 0, sizeof(term->tab_stops));
    else if (ps == 0)
        term->tab_stops[term->cur->x >> 3] &= (uint8_t)~(1u << (term->cur->x & 7));
}

/* ---- cursor movement --------------------------------------------------- */

// Up n rows. A cursor inside the region stops at its top margin; above it, at
// the top row.
static void term_cursor_up(term_state_t *term, uint16_t n)
{
    uint8_t fence = (term->cur->y >= term->margin_top) ? term->margin_top : 0;
    uint8_t y = term->cur->y;
    uint16_t room = (uint16_t)(y - fence);
    term_goto(term, term->cur->x, (unsigned)(y - (n < room ? n : room)));
}

// Down n rows. A cursor inside the region stops at its bottom margin; below
// it, at the last row.
static void term_cursor_down(term_state_t *term, uint16_t n)
{
    uint8_t fence = (term->cur->y <= term->margin_bot) ? term->margin_bot
                                                       : (uint8_t)(term->height - 1);
    uint8_t y = term->cur->y;
    uint16_t room = (uint16_t)(fence - y);
    term_goto(term, term->cur->x, (unsigned)(y + (n < room ? n : room)));
}

static void term_cursor_forward(term_state_t *term, uint16_t n)
{
    term_goto(term, (unsigned)term->cur->x + n, term->cur->y);
}

static void term_cursor_back(term_state_t *term, uint16_t n)
{
    uint8_t x = term->cur->x;
    term_goto(term, n < x ? (unsigned)(x - n) : 0u, term->cur->y);
}

// Translate a 1-indexed row from a CSI parameter into an absolute 0-indexed
// row, honoring DECOM. In origin mode the row is clamped inside the scroll
// region; otherwise term_goto clamps it to the screen.
static unsigned term_origin_row(const term_state_t *term, uint16_t row)
{
    if (!term->cur->origin_mode)
        return (unsigned)row - 1u;
    unsigned region_h = (unsigned)(term->margin_bot - term->margin_top + 1);
    if (row > region_h)
        row = (uint16_t)region_h;
    return (unsigned)term->margin_top + row - 1u;
}

// Home: the top left, or the region's under DECOM.
static void term_home(term_state_t *term)
{
    term_goto(term, 0, term->cur->origin_mode ? term->margin_top : 0);
}

static void term_out_CUP(term_state_t *term)
{
    term_goto(term, term_param(term, 1, 1) - 1u,
              term_origin_row(term, term_param(term, 0, 1)));
}

static void term_out_VPA(term_state_t *term)
{
    term_goto(term, term->cur->x, term_origin_row(term, term_param(term, 0, 1)));
}

/* ---- erasing and editing ----------------------------------------------- */

// Erase Line: 0 cursor to end, 1 start to cursor, 2 the whole line.
static void term_erase_line(term_state_t *term, uint16_t mode)
{
    cursor_state_t *cur = term->cur;
    cur->wrap_pending = false;
    unsigned from = 0, to = term->width;
    if (mode == 0)
        from = cur->x;
    else if (mode == 1)
        to = (unsigned)cur->x + 1u;
    else if (mode != 2)
        return;
    fill_cells(term_row_ptr(term, cur->y), from, to, cur->fg_color, cur->bg_color);
}

// Erase Display. ED 0 and ED 1 erase the cursor's row like EL 0 and EL 1 and
// lazy-erase the rows past it. ED 2 lazy-erases every row and cleans the
// cursor's. ED 3 (xterm) forgets the scrollback: the rows a taller view would
// bring back.
static void term_erase_display(term_state_t *term, uint16_t mode)
{
    cursor_state_t *cur = term->cur;
    switch (mode)
    {
    case 0:
        term_mark_rows_erase(term, (uint8_t)(cur->y + 1), term->height);
        term_erase_line(term, 0);
        break;
    case 1:
        term_mark_rows_erase(term, 0, cur->y);
        term_erase_line(term, 1);
        break;
    case 2:
        cur->wrap_pending = false;
        term_mark_rows_erase(term, 0, term->height);
        term_clean_line(term, cur->y);
        break;
    case 3:
        cur->wrap_pending = false;
        term->screen->hidden = 0;
        break;
    }
}

// Delete characters: the rest of the row moves left; erased cells come in at
// the right.
static void term_out_DCH(term_state_t *term)
{
    cursor_state_t *cur = term->cur;
    cur->wrap_pending = false;
    unsigned avail = (unsigned)(term->width - cur->x);
    unsigned n = term_param(term, 0, 1);
    if (n > avail)
        n = avail;
    term_data_t *row = term_row_ptr(term, cur->y);
    memmove(&row[cur->x], &row[cur->x + n], (avail - n) * sizeof(term_data_t));
    fill_cells(row, term->width - n, term->width, cur->fg_color, cur->bg_color);
}

// Insert Pn erased characters at the cursor; cells past the right edge are
// pushed off the row and lost.
static void term_out_ICH(term_state_t *term)
{
    cursor_state_t *cur = term->cur;
    cur->wrap_pending = false;
    unsigned avail = (unsigned)(term->width - cur->x);
    unsigned n = term_param(term, 0, 1);
    if (n > avail)
        n = avail;
    term_data_t *row = term_row_ptr(term, cur->y);
    memmove(&row[cur->x + n], &row[cur->x], (avail - n) * sizeof(term_data_t));
    fill_cells(row, cur->x, cur->x + n, cur->fg_color, cur->bg_color);
}

// Erase Pn characters from the cursor. The cursor doesn't move.
static void term_out_ECH(term_state_t *term)
{
    cursor_state_t *cur = term->cur;
    cur->wrap_pending = false;
    unsigned avail = (unsigned)(term->width - cur->x);
    unsigned n = term_param(term, 0, 1);
    if (n > avail)
        n = avail;
    fill_cells(term_row_ptr(term, cur->y), cur->x, cur->x + n, cur->fg_color, cur->bg_color);
}

// Insert or delete Pn lines at the cursor, inside the region only; the cursor
// goes to the first column (xterm and Linux).
static void term_out_IL_DL(term_state_t *term, bool insert)
{
    uint8_t y = term->cur->y;
    if (y < term->margin_top || y > term->margin_bot)
        return;
    uint16_t n = term_param(term, 0, 1);
    if (insert)
        term_region_scroll_down(term, y, term->margin_bot, n);
    else
        term_region_scroll_up(term, y, term->margin_bot, n);
    term_goto(term, 0, y);
}

// Scroll the region up (SU) or down (SD) by Pn rows. The cursor stays.
static void term_out_SU_SD(term_state_t *term, bool up)
{
    uint16_t n = term_param(term, 0, 1);
    if (up)
        term_region_scroll_up(term, term->margin_top, term->margin_bot, n);
    else
        term_region_scroll_down(term, term->margin_top, term->margin_bot, n);
    term_clean_line(term, term->cur->y);
}

/* ---- saved cursor and screens ------------------------------------------ */

// DECSC (ESC 7, CSI s, ?1048 h): save the cursor, its pen, the pending wrap
// and the character sets in the active screen's slot. Each screen has its own.
static void term_save_cursor(term_state_t *term)
{
    term->screen->saved = *term->cur;
    term->screen->has_saved = true;
}

// DECRC (ESC 8, CSI u, ?1048 l): restore them. With nothing saved the slot
// holds the defaults: home, SGR 0, ASCII, origin mode off.
static void term_restore_cursor(term_state_t *term)
{
    *term->cur = term->screen->saved;
    bool pending = term->cur->wrap_pending;
    term_goto(term, term->cur->x, term->cur->y);
    term->cur->wrap_pending = pending;
}

// Swap to the buffer at idx (0 = primary, 1 = alt). Both screen and cur
// pointers move together.
static inline void term_set_screen(term_state_t *term, uint8_t idx)
{
    term->screen = &term->bufs[idx];
    term->cur = &term->screen->cs;
    term->alt_active = (idx != 0);
}

// Enter the alternate screen. The cursor and its pen come along, as in xterm,
// where they belong to the terminal and not to a screen. ?1049 clears it.
static void term_enter_alt(term_state_t *term, bool clear)
{
#if TERM_ALT_SCREEN
    if (!term->alt_active)
    {
        term->bufs[1].cs = term->bufs[0].cs;
        term_set_screen(term, 1);
    }
    if (clear)
        term_mark_buf_erased(term->screen, term->height,
                             term->cur->fg_color, term->cur->bg_color);
    term_goto(term, term->cur->x, term->cur->y);
#else
    /* No second buffer on this platform: a full-screen program draws on the
     * primary and leaves its output behind, as on a terminal without one. */
    (void)term;
    (void)clear;
#endif
}

// Leave the alternate screen, the cursor coming back along with it. ?1047
// clears the alternate screen first; ?1049 restores the cursor instead.
static void term_leave_alt(term_state_t *term, bool clear, bool restore)
{
#if TERM_ALT_SCREEN
    if (!term->alt_active)
    {
        // ?1049 l on the primary screen still restores the cursor, as in
        // xterm.
        if (restore)
            term_restore_cursor(term);
        return;
    }
    if (clear)
        term_mark_buf_erased(term->screen, term->height,
                             term->cur->fg_color, term->cur->bg_color);
    if (!restore)
        term->bufs[0].cs = term->bufs[1].cs;
    term_set_screen(term, 0);
#else
    (void)clear;
#endif
    if (restore)
        term_restore_cursor(term);
    else
        term_goto(term, term->cur->x, term->cur->y);
}

/* ---- SGR --------------------------------------------------------------- */

// Read the color of SGR 38, 48 or 58 starting at param i, with `subs` colon
// sub-parameters after it. Returns how many params after i were taken; kind
// is TERM_COLOR_DEFAULT when they named no color, in which case the pen keeps
// its color.
//   38;5;N  38:5:N            palette N; nothing for N > 255
//   38;2;r;g;b  38:2::r:g:b  38:2:r:g:b   truecolor; nothing for a channel > 255
//   38;1  38:1               transparent (ours)
// An unknown or cut-off form is taken whole, so its numbers are never run as
// SGR codes of their own.
static uint8_t term_sgr_color(const term_state_t *term, uint8_t i, uint8_t subs,
                              uint8_t *kind, uint16_t *value)
{
    const uint16_t *p = term->csi_param;
    uint8_t avail = subs ? subs : (uint8_t)(term->csi_param_count - 1 - i);
    *kind = TERM_COLOR_DEFAULT;
    if (!avail)
        return 0;
    uint8_t want = 1;
    const uint16_t *rgb = NULL;
    switch (p[i + 1])
    {
    case 5:
        want = 2;
        if (avail >= 2 && p[i + 2] < 256)
        {
            *kind = TERM_COLOR_INDEX;
            *value = p[i + 2];
        }
        break;
    case 2:
        want = 4;
        if (subs >= 5)
            rgb = &p[i + 3];
        else if (avail >= 4)
            rgb = &p[i + 2];
        if (rgb && rgb[0] < 256 && rgb[1] < 256 && rgb[2] < 256)
        {
            *kind = TERM_COLOR_RGB;
            *value = SCANVIDEO_ALPHA_MASK | SCANVIDEO_PIXEL_FROM_RGB8(rgb[0], rgb[1], rgb[2]);
        }
        break;
    case 1:
        *kind = TERM_COLOR_RGB;
        *value = 0; // the caller makes the base color transparent
        break;
    }
    if (subs)
        return subs;
    return want < avail ? want : avail;
}

static void term_sgr_reset(term_state_t *term)
{
    cursor_state_t *cur = term->cur;
    cur->bold = false;
    cur->faint = false;
    cur->reverse = false;
    cur->conceal = false;
    cur->sgr_attr = 0;
    cur->fg_kind = TERM_COLOR_DEFAULT;
    cur->bg_kind = TERM_COLOR_DEFAULT;
    cur->ul_color = 0;
    cur->underline_color_set = false;
}

static void term_out_SGR(term_state_t *term)
{
    cursor_state_t *cur = term->cur;
    uint8_t count = term->csi_param_count;
    for (uint8_t i = 0; i < count;)
    {
        uint16_t param = term->csi_param[i];
        // The colon sub-parameters that belong to this one.
        uint8_t subs = 0;
        while (i + subs + 1 < count && term->csi_separator[i + subs] == ':')
            subs++;
        uint8_t used = subs;
        switch (param)
        {
        case 0: // reset
            term_sgr_reset(term);
            break;
        case 1: // bold intensity
            cur->bold = true;
            break;
        case 2: // faint -- halve channel brightness at SGR time.
            cur->faint = true;
            break;
        case 3: // italic (renders only in 8x16 mode; 8x8 has no italic font)
            cur->sgr_attr |= TERM_ATTR_ITALIC;
            break;
        case 4: // underline; kitty's 4:n picks a style
        {
            uint16_t style = subs ? term->csi_param[i + 1] : 1;
            cur->sgr_attr &= (uint8_t)~(TERM_ATTR_UNDERLINE | TERM_ATTR_DBL_UL);
            if (style == 2)
                cur->sgr_attr |= TERM_ATTR_DBL_UL;
            else if (style >= 1 && style <= 5)
                cur->sgr_attr |= TERM_ATTR_UNDERLINE;
            break;
        }
        case 5: // slow blink (~1.5 Hz)
            cur->sgr_attr = (uint8_t)((cur->sgr_attr & ~TERM_ATTR_ANY_BLINK) | TERM_ATTR_BLINK);
            break;
        case 6: // rapid blink (~3 Hz)
            cur->sgr_attr = (uint8_t)((cur->sgr_attr & ~TERM_ATTR_ANY_BLINK) | TERM_ATTR_BLINK_FAST);
            break;
        case 7: // reverse
            cur->reverse = true;
            break;
        case 8: // conceal
            cur->conceal = true;
            break;
        case 9: // strikethrough
            cur->sgr_attr |= TERM_ATTR_STRIKE;
            break;
        case 10: // primary font (SCO, Linux): controls act
            term->disp_ctrl = false;
            break;
        case 11: // first alternate font (SCO, Linux): controls are drawn
            term->disp_ctrl = true;
            break;
        case 21: // double underline (ECMA-48, xterm)
            cur->sgr_attr |= TERM_ATTR_DBL_UL;
            break;
        case 22: // normal intensity -- cancels both bold and faint
            cur->bold = false;
            cur->faint = false;
            break;
        case 23: // italic off
            cur->sgr_attr &= (uint8_t)~TERM_ATTR_ITALIC;
            break;
        case 24: // underline off
            cur->sgr_attr &= (uint8_t)~(TERM_ATTR_UNDERLINE | TERM_ATTR_DBL_UL);
            break;
        case 25: // not blink (clears slow and rapid)
            cur->sgr_attr &= (uint8_t)~TERM_ATTR_ANY_BLINK;
            break;
        case 27: // reverse off
            cur->reverse = false;
            break;
        case 28: // conceal off
            cur->conceal = false;
            break;
        case 29: // strikethrough off
            cur->sgr_attr &= (uint8_t)~TERM_ATTR_STRIKE;
            break;
        case 30:
        case 31:
        case 32:
        case 33: // foreground color
        case 34:
        case 35:
        case 36:
        case 37:
            cur->fg_kind = TERM_COLOR_INDEX;
            cur->fg_index = (uint8_t)(param - 30);
            break;
        case 38:
        case 48:
        case 58:
        {
            uint8_t kind;
            uint16_t value;
            used = term_sgr_color(term, i, subs, &kind, &value);
            if (kind == TERM_COLOR_RGB && !value)
            {
                // SGR 38;1: the base color made transparent.
                uint16_t base = param == 48 ? term_bg_base(term, cur)
                                : param == 58 && cur->underline_color_set
                                    ? cur->ul_color
                                    : term_fg_base(term, cur);
                value = (uint16_t)(base & ~SCANVIDEO_ALPHA_MASK);
            }
            if (kind == TERM_COLOR_DEFAULT)
                break;
            if (param == 58)
            {
                cur->ul_color = kind == TERM_COLOR_INDEX ? color_256_term[value] : value;
                cur->underline_color_set = true;
            }
            else if (param == 38)
            {
                cur->fg_kind = kind;
                cur->fg_index = (uint8_t)value;
                cur->fg_rgb = value;
            }
            else
            {
                cur->bg_kind = kind;
                cur->bg_index = (uint8_t)value;
                cur->bg_rgb = value;
            }
            break;
        }
        case 39: // default foreground
            cur->fg_kind = TERM_COLOR_DEFAULT;
            break;
        case 40:
        case 41:
        case 42:
        case 43: // background color
        case 44:
        case 45:
        case 46:
        case 47:
            cur->bg_kind = TERM_COLOR_INDEX;
            cur->bg_index = (uint8_t)(param - 40);
            break;
        case 49: // default background
            cur->bg_kind = TERM_COLOR_DEFAULT;
            break;
        case 53: // overline
            cur->sgr_attr |= TERM_ATTR_OVERLINE;
            break;
        case 55: // overline off
            cur->sgr_attr &= (uint8_t)~TERM_ATTR_OVERLINE;
            break;
        case 59: // default underline color
            cur->underline_color_set = false;
            break;
        case 90:
        case 91:
        case 92:
        case 93: // bright foreground color
        case 94:
        case 95:
        case 96:
        case 97:
            // The bright palette entry itself, so SGR 22 leaves it bright --
            // xterm behavior.
            cur->fg_kind = TERM_COLOR_INDEX;
            cur->fg_index = (uint8_t)(param - 90 + 8);
            break;
        case 100:
        case 101:
        case 102:
        case 103: // bright background color
        case 104:
        case 105:
        case 106:
        case 107:
            cur->bg_kind = TERM_COLOR_INDEX;
            cur->bg_index = (uint8_t)(param - 100 + 8);
            break;
        }
        i = (uint8_t)(i + 1 + used);
    }
    term_resolve_pen(term, cur);
}

/* ---- replies ----------------------------------------------------------- */

// Only the term currently being rendered should reply to host queries
static bool term_is_visible(term_state_t *term)
{
    return term->width == vga_term_columns(vga_canvas_width());
}

static void term_reply(term_state_t *term, const char *s, size_t n)
{
    if (term_is_visible(term))
        manifold_reply(s, n);
}

static void term_replyf(term_state_t *term, const char *fmt, unsigned a,
                        unsigned b, unsigned c)
{
    if (!term_is_visible(term))
        return;
    char buf[TERM_REPLY_MAX];
    int n = snprintf(buf, sizeof(buf), fmt, a, b, c);
    if (n > 0 && n < (int)sizeof(buf))
        manifold_reply(buf, (size_t)n);
}

#define TERM_REPLY(term, lit) term_reply((term), (lit), sizeof(lit) - 1)

// Device Status Report: 5 status, 6 cursor position.
static void term_out_DSR(term_state_t *term)
{
    switch (term->csi_param[0])
    {
    case 5:
        TERM_REPLY(term, "\33[0n");
        break;
    case 6:
    {
        unsigned y = term->cur->y;
        // Region-relative under DECOM, guarded so a cursor above the region
        // reports row 1 rather than underflowing.
        if (term->cur->origin_mode)
            y = (y >= term->margin_top) ? (y - term->margin_top) : 0;
        term_replyf(term, "\33[%u;%uR", y + 1, term->cur->x + 1u, 0);
        break;
    }
    }
}

// The pixel height of one character cell: 8x8 on the 320-wide canvases, where
// the 40-column terminal is drawn, 8x16 on the others.
static unsigned term_cell_height(const term_state_t *term)
{
    return term->width == 40 ? 8 : 16;
}

// xterm's window reports, CSI Ps t, of the ones that mean something here.
// Linux answers none of them.
static void term_out_window(term_state_t *term)
{
    if (term->csi_param_count > 1)
        return;
    switch (term->csi_param[0])
    {
    case 11: // iconified? never
        TERM_REPLY(term, "\33[1t");
        break;
    case 14: // text area in pixels
        term_replyf(term, "\33[4;%u;%ut", term->height * term_cell_height(term),
                    term->width * 8u, 0);
        break;
    case 16: // character cell in pixels
        term_replyf(term, "\33[6;%u;%ut", term_cell_height(term), 8, 0);
        break;
    case 18: // text area in characters
        term_replyf(term, "\33[8;%u;%ut", term->height, term->width, 0);
        break;
    case 19: // screen in characters
        term_replyf(term, "\33[9;%u;%ut", term->height, term->width, 0);
        break;
    }
}

// Request Mode (DECRQM). Reply: CSI ? Ps ; Pm $ y (private) or CSI Ps ; Pm $ y
// (ANSI). Pm: 0 not recognized, 1 set, 2 reset, 3 permanently set,
// 4 permanently reset.
static void term_out_DECRQM(term_state_t *term, bool private)
{
    unsigned ps = term->csi_param[0];
    unsigned pm = 0;
    if (private)
    {
        switch (ps)
        {
        case 2: // DECANM: ANSI mode, since this reply is ANSI
            pm = 1;
            break;
        case 3: // DECCOLM: 80 columns only
        case 4: // DECSCLM: no smooth scroll
            pm = 4;
            break;
        case 6: // DECOM
            pm = term->cur->origin_mode ? 1 : 2;
            break;
        case 7: // DECAWM
            pm = term->autowrap ? 1 : 2;
            break;
        case 12: // cursor blink
            pm = (term->cursor_style == 2 || term->cursor_style == 4 ||
                  term->cursor_style == 6)
                     ? 2
                     : 1;
            break;
        case 25: // DECTCEM
            pm = term->cursor_enabled ? 1 : 2;
            break;
        case 33: // iCE colors
            pm = term->ice_colors ? 1 : 2;
            break;
        case 47:
        case 1047:
        case 1049:
            pm = term->alt_active ? 1 : 2;
            break;
        case 1048:
            pm = term->screen->has_saved ? 1 : 2;
            break;
        }
    }
    else
    {
        switch (ps)
        {
        case 2: // KAM: the keyboard is never locked
            pm = 4;
            break;
        case 3: // CRM: draw the controls
            pm = term->disp_ctrl ? 1 : 2;
            break;
        case 4: // IRM
            pm = term->insert_mode ? 1 : 2;
            break;
        case 12: // SRM: no local echo
            pm = 3;
            break;
        case 20: // LNM
            pm = term->newline_mode ? 1 : 2;
            break;
        }
    }
    term_replyf(term, private ? "\33[?%u;%u$y" : "\33[%u;%u$y", ps, pm, 0);
}

// DECREQTPARM, CSI 0 x or CSI 1 x: the VT100's line settings, answered as a
// VT100 on its fastest line does -- no parity, 8 bits, 9600 baud both ways, clock
// multiplier 1, no flags. CSI 0 x asks for a report unsolicited reports may
// follow (2); CSI 1 x, for one only on request (3).
static void term_out_DECREQTPARM(term_state_t *term)
{
    uint16_t ps = term->csi_param[0];
    if (ps <= 1)
        term_replyf(term, "\33[%u;1;1;112;112;1;0x", ps + 2u, 0, 0);
}

/* ---- modes ------------------------------------------------------------- */

// ?12 turns the cursor's blink on or off, as xterm does. It shares the
// DECSCUSR style: 2, 4 and 6 are the steady styles.
static void term_set_cursor_blink(term_state_t *term, bool blink)
{
    uint8_t s = term->cursor_style;
    if (blink && (s == 2 || s == 4 || s == 6))
        s--;
    else if (!blink && s == 0)
        s = 2;
    else if (!blink && (s & 1))
        s++;
    if (s != term->cursor_style)
    {
        term->cursor_style = s;
        term_cursor_restart_blink(term);
    }
}

// Set cursor style (CSI Ps SP q -- DECSCUSR). 0 = host default, 1/2 = block,
// 3/4 = underline, 5/6 = bar. Values past 6 are ignored.
static void term_out_DECSCUSR(term_state_t *term)
{
    uint16_t ps = term->csi_param[0];
    if (ps > 6 || term->cursor_style == ps)
        return;
    term->cursor_style = (uint8_t)ps;
    term_cursor_restart_blink(term);
}

// Set Top and Bottom Margins (CSI Pt ; Pb r). The cursor goes home.
static void term_out_DECSTBM(term_state_t *term)
{
    uint16_t top = term_param(term, 0, 1);
    uint16_t bot = term_param(term, 1, term->height);
    if (bot > term->height)
        bot = term->height;
    if (top >= bot)
        return; // invalid region: ignore (VT100)
    term->margin_top = (uint8_t)(top - 1);
    term->margin_bot = (uint8_t)(bot - 1);
    term_home(term);
}

// SM and RM, the ANSI modes.
static void term_out_SM_RM(term_state_t *term, bool set)
{
    for (uint8_t i = 0; i < term->csi_param_count; i++)
    {
        switch (term->csi_param[i])
        {
        case 3: // CRM: draw the controls, as SGR 11
            term->disp_ctrl = set;
            break;
        case 4: // IRM: insert mode
            term->insert_mode = set;
            break;
        case 20: // LNM: new line mode
            term->newline_mode = set;
            break;
            // KAM (2) and SRM (12) are taken and ignored.
        }
    }
}

// DECSET and DECRST, the private modes.
static void term_out_DECSET(term_state_t *term, bool set)
{
    for (uint8_t i = 0; i < term->csi_param_count; i++)
    {
        switch (term->csi_param[i])
        {
        case 2: // DECANM: reset enters VT52 mode; ESC < leaves it
            if (!set)
            {
                term->vt52 = true;
                term->cur->g0_charset = TERM_CS_ASCII;
                term->cur->g1_charset = TERM_CS_ASCII;
                term->cur->gl_is_g1 = false;
            }
            break;
        case 6: // DECOM
            term->cur->origin_mode = set;
            term_home(term);
            break;
        case 7: // DECAWM (autowrap)
            term->autowrap = set;
            break;
        case 12: // cursor blink (AT&T 610)
            term_set_cursor_blink(term, set);
            break;
        case 25: // DECTCEM
            if (set && !term->cursor_enabled)
                term_cursor_restart_blink(term);
            term->cursor_enabled = set;
            break;
        case 33: /* iCE colors -- SGR 5/6 becomes the IBM-VGA bright-bg hack */
            term->ice_colors = set;
            term_resolve_all_pens(term);
            break;
        case 47: /* the alternate screen, swap only */
            if (set)
                term_enter_alt(term, false);
            else
                term_leave_alt(term, false, false);
            break;
        case 1047: /* the alternate screen, cleared on leaving */
            if (set)
                term_enter_alt(term, false);
            else
                term_leave_alt(term, true, false);
            break;
        case 1048: /* DECSC and DECRC */
            if (set)
                term_save_cursor(term);
            else
                term_restore_cursor(term);
            break;
        case 1049: /* DECSC, then the alternate screen cleared (the modern default) */
            if (set)
            {
                term_save_cursor(term);
                term_enter_alt(term, true);
            }
            else
                term_leave_alt(term, false, true);
            break;
        }
    }
}

/* ---- OSC --------------------------------------------------------------- */

static int term_hex(char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

// Read `len` hex digits at s, or -1 when one is not.
static long term_hex_run(const char *s, unsigned len)
{
    long v = 0;
    for (unsigned i = 0; i < len; i++)
    {
        int h = term_hex(s[i]);
        if (h < 0)
            return -1;
        v = v * 16 + h;
    }
    return v;
}

// An X11 color spec, the ones xterm takes for its dynamic colors: #rgb,
// #rrggbb, #rrrgggbbb, #rrrrggggbbbb, or rgb:r/g/b with 1-4 hex digits each.
static bool term_parse_color(const char *s, unsigned len, uint16_t *color)
{
    unsigned rgb[3];
    if (len >= 4 && s[0] == '#' && (len - 1) % 3 == 0 && len <= 13)
    {
        unsigned n = (len - 1) / 3;
        for (int c = 0; c < 3; c++)
        {
            long v = term_hex_run(&s[1 + c * n], n);
            if (v < 0)
                return false;
            rgb[c] = (unsigned)(n == 1 ? v * 17 : v >> (4 * (n - 2)));
        }
    }
    else if (len > 4 && !memcmp(s, "rgb:", 4))
    {
        unsigned at = 4;
        for (int c = 0; c < 3; c++)
        {
            unsigned n = 0;
            while (at + n < len && s[at + n] != '/')
                n++;
            long v = (n >= 1 && n <= 4) ? term_hex_run(&s[at], n) : -1;
            if (v < 0)
                return false;
            rgb[c] = (unsigned)(v * 255 / ((1L << (4 * n)) - 1));
            at += n + 1;
        }
        if (at != len + 1)
            return false;
    }
    else
        return false;
    *color = SCANVIDEO_ALPHA_MASK | SCANVIDEO_PIXEL_FROM_RGB8(rgb[0], rgb[1], rgb[2]);
    return true;
}

// A palette index of 0-255 in decimal, or -1.
static int term_parse_index(const char *s, unsigned len)
{
    if (!len || len > 3)
        return -1;
    int v = 0;
    for (unsigned i = 0; i < len; i++)
    {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v <= 255 ? v : -1;
}

// One ';'-separated item of an OSC, applied as it ends.
//   OSC 4 ; N ; spec ; N ; spec ...   set palette entries
//   OSC 104 ; N ; N ...               reset palette entries
//   OSC 10 ; fg ; bg ; cursor         set the dynamic colors, each item the
//                                     next one, as xterm does
static void term_osc_item(term_state_t *term)
{
    unsigned len = term->osc_len;
    if (len > TERM_OSC_SPEC_MAX)
        len = 0; // too long: no item matches an empty one
    const char *s = term->osc_spec;
    uint16_t color;
    switch (term->osc_ps)
    {
    case 4:
        if (!(term->osc_item & 1))
        {
            int idx = term_parse_index(s, len);
            term->osc_index = idx < 0 ? 0xFFFF : (uint16_t)idx;
        }
        else if (term->osc_index < 256 && term_parse_color(s, len, &color))
            color_256_term[term->osc_index] = color;
        break;
    case 104:
    {
        int idx = term_parse_index(s, len);
        if (idx >= 0)
            color_256_term[idx] = color_256[idx];
        break;
    }
    case 10:
    case 11:
    case 12:
        if (!term_parse_color(s, len, &color))
            break;
        switch (term->osc_ps + term->osc_item)
        {
        case 10:
            term->default_fg_color = color;
            break;
        case 11:
            term->default_bg_color = color;
            break;
        case 12:
            term->cursor_color = color;
            break;
        }
        break;
    }
}

// The OSC ended with BEL or ST: its last item, then the commands that act on
// the whole of it.
//   OSC 104   reset the palette     OSC 110   reset the default foreground
//   OSC 111   the default background  OSC 112   the cursor color
// Other commands, such as the titles, are taken and ignored.
static void term_osc_end(term_state_t *term)
{
    term->rep_valid = false;
    if (term->osc_stage == 1)
        term_osc_item(term);
    else if (term->osc_stage == 0)
    {
        switch (term->osc_ps)
        {
        case 104:
            memcpy(color_256_term, color_256, sizeof(color_256_term));
            break;
        case 110:
            term->default_fg_color = color_256[TERM_FG_COLOR_INDEX];
            break;
        case 111:
            term->default_bg_color = color_256[TERM_BG_COLOR_INDEX];
            break;
        case 112:
            term->cursor_color = color_256[TERM_FG_COLOR_INDEX];
            break;
        }
    }
    term_resolve_all_pens(term);
}

static void term_osc_byte(term_state_t *term, uint8_t ch)
{
    if (ch == '\a')
    {
        term->ansi_state = ansi_state_ground;
        term_osc_end(term);
        return;
    }
    if (ch < 0x20)
        return; // other controls inside a string are ignored
    switch (term->osc_stage)
    {
    case 0: // the command number
        if (ch >= '0' && ch <= '9')
        {
            uint32_t v = term->osc_ps * 10u + (ch - '0');
            term->osc_ps = v > 0xFFFF ? 0xFFFF : (uint16_t)v;
            term->osc_len = 1;
        }
        else if (ch == ';')
        {
            term->osc_stage = 1;
            term->osc_len = 0;
            term->osc_item = 0;
        }
        else if (!term->osc_len && ch == 'P')
        {
            // Linux: ESC ] P nrrggbb sets palette entry n, with no terminator.
            term->ansi_state = ansi_state_osc_palette;
            term->osc_len = 0;
        }
        else if (!term->osc_len && ch == 'R')
        {
            // Linux: ESC ] R resets palette 0-15, with no terminator.
            memcpy(color_256_term, color_256, 16 * sizeof(color_256_term[0]));
            term->ansi_state = ansi_state_ground;
            term_resolve_all_pens(term);
        }
        else
            term->osc_stage = 2;
        break;
    case 1: // the items
        if (ch == ';')
        {
            term_osc_item(term);
            term->osc_len = 0;
            if (term->osc_item < 0xFF)
                term->osc_item++;
        }
        else if (term->osc_len < TERM_OSC_SPEC_MAX)
            term->osc_spec[term->osc_len++] = (char)ch;
        else
            term->osc_len = TERM_OSC_SPEC_MAX + 1;
        break;
    }
}

// Linux's ESC ] P nrrggbb: the palette entry n (0-f) and its color, seven
// hex digits and no terminator. Anything else ends it, and is dropped.
static void term_osc_palette_byte(term_state_t *term, uint8_t ch)
{
    if (term_hex((char)ch) < 0)
    {
        term->ansi_state = ansi_state_ground;
        return;
    }
    term->osc_spec[term->osc_len++] = (char)ch;
    if (term->osc_len < 7)
        return;
    term->ansi_state = ansi_state_ground;
    term->rep_valid = false;
    const char *s = term->osc_spec;
    color_256_term[term_hex(s[0])] =
        SCANVIDEO_ALPHA_MASK | SCANVIDEO_PIXEL_FROM_RGB8(term_hex_run(&s[1], 2),
                                                         term_hex_run(&s[3], 2),
                                                         term_hex_run(&s[5], 2));
    term_resolve_all_pens(term);
}

/* ---- the parser -------------------------------------------------------- */

// The C0 controls that act: from the ground state, and from inside a
// sequence for the ones in TERM_C0_IN_SEQ. NUL and BEL do nothing here; the
// console's bell is the RIA's.
static void term_execute(term_state_t *term, uint8_t ch)
{
    term->rep_valid = false;
    switch (ch)
    {
    case '\b': // BS
        term->cur->wrap_pending = false;
        if (term->cur->x > 0)
            term->cur->x--;
        break;
    case '\t': // HT
        term_tab_forward(term, 1);
        break;
    case '\n': // LF
    case '\v': // VT
    case '\f': // FF: all three are LF, as on a VT102 and in xterm and Linux
        term_out_LF(term);
        break;
    case '\r': // CR
        term_out_CR(term);
        break;
    case 0x0E: // SO (Shift Out): activate G1
        term->cur->gl_is_g1 = true;
        break;
    case 0x0F: // SI (Shift In): activate G0
        term->cur->gl_is_g1 = false;
        break;
    }
}

// A byte in the ground state. Bytes 0x20-0x7E and 0x80-0xFF are glyphs. A C0
// control Linux has no action for is drawn too, and with SGR 11 (or CRM)
// every one but NUL, BS, LF, FF, CR, SO, SI and ESC is, DEL included. ENQ is
// drawn: there is no answerback.
static void term_ground(term_state_t *term, uint8_t ch)
{
    if (ch >= 0x20 && ch != TERM_DEL)
        term_print(term, ch);
    else if (ch == TERM_DEL)
    {
        if (term->disp_ctrl)
            term_print(term, ch);
    }
    else if (term->disp_ctrl ? !(TERM_C0_ALWAYS & (1u << ch))
                             : !(TERM_C0_ACTION & (1u << ch)))
        term_print(term, ch);
    else
        term_execute(term, ch);
}

// A control or a byte past 0x7E inside an escape sequence. The controls of
// TERM_C0_IN_SEQ act and the sequence goes on; NUL and DEL are ignored; any
// other ends the sequence, silently, as in Linux. Returns false for an
// ordinary sequence byte. CAN, SUB and ESC never get here.
static bool term_seq_control(term_state_t *term, uint8_t ch)
{
    if (ch >= 0x20 && ch < TERM_DEL)
        return false;
    if (ch && ch != TERM_DEL)
    {
        if (ch < 0x20 && (TERM_C0_IN_SEQ & (1u << ch)))
            term_execute(term, ch);
        else
            term->ansi_state = ansi_state_ground;
    }
    return true;
}

// ESC ( F and ESC ) F: designate G0 or G1. U and K, Linux's null and user
// maps, are ASCII here; the national sets and DEC's alternate ROMs are left
// as they were.
static void term_designate(uint8_t *g, uint8_t final)
{
    switch (final)
    {
    case 'B':
    case 'U':
    case 'K':
        *g = TERM_CS_ASCII;
        break;
    case '0':
        *g = TERM_CS_DEC;
        break;
    case 'A':
        *g = TERM_CS_UK;
        break;
    }
}

// The final byte of ESC I... F, a sequence with intermediates. The character
// set designations are the only ones that act; ESC # (line sizes and DECALN),
// ESC % (UTF-8), ESC SP (7/8-bit controls), G2 and G3 are taken and ignored.
static void term_esc_int_final(term_state_t *term, uint8_t ch)
{
    term->ansi_state = ansi_state_ground;
    term->rep_valid = false;
    if (term->csi_intermediate == '(')
        term_designate(&term->cur->g0_charset, ch);
    else if (term->csi_intermediate == ')')
        term_designate(&term->cur->g1_charset, ch);
}

static void term_esc_int_byte(term_state_t *term, uint8_t ch)
{
    if (term_seq_control(term, ch))
        return;
    if (ch < 0x30)
        term->csi_intermediate = 0xFF; // a second intermediate: nothing we know
    else
        term_esc_int_final(term, ch);
}

static void term_start_csi(term_state_t *term)
{
    term->ansi_state = ansi_state_csi;
    term->csi_param_count = 0;
    term->csi_param[0] = 0;
    term->csi_separator[0] = 0;
    term->csi_private = 0;
    term->csi_intermediate = 0;
    term->csi_colon = false;
    term->csi_any = false;
}

static void term_start_osc(term_state_t *term)
{
    term->ansi_state = ansi_state_osc;
    term->osc_ps = 0;
    term->osc_stage = 0;
    term->osc_len = 0;
    term->osc_item = 0;
}

static void term_esc_byte(term_state_t *term, uint8_t ch)
{
    bool st = term->osc_pending;
    term->osc_pending = false;
    if (st && ch != '\\')
        term_resolve_all_pens(term); // an OSC cut off by a new sequence
    if (term_seq_control(term, ch))
        return;
    if (ch < 0x30)
    {
        term->ansi_state = ansi_state_esc_int;
        term->csi_intermediate = ch;
        return;
    }
    term->ansi_state = ansi_state_ground;
    if (ch != '[' && ch != ']')
        term->rep_valid = false;
    switch (ch)
    {
    case '[': // CSI
        term_start_csi(term);
        break;
    case ']': // OSC
        term_start_osc(term);
        break;
    case 'P': // DCS
    case 'X': // SOS
    case '^': // PM
    case '_': // APC
        term->ansi_state = ansi_state_string;
        break;
    case '\\': // ST: the end of a string, which ends an OSC
        if (st)
            term_osc_end(term);
        break;
    case 'c': // RIS
        term_out_RIS(term);
        break;
    case 'D': // IND - Index (move down, scroll on margin_bot)
        term_index(term);
        break;
    case 'E': // NEL - Next Line (CR + IND)
        term_out_CR(term);
        term_index(term);
        break;
    case 'H': // HTS - Horizontal Tab Set at current column
        term->tab_stops[term->cur->x >> 3] |= 1u << (term->cur->x & 7);
        break;
    case 'M': // RI
        term_reverse_index(term);
        break;
    case '7': // DECSC
        term_save_cursor(term);
        break;
    case '8': // DECRC
        term_restore_cursor(term);
        break;
    case 'Z': // DECID: answered as Primary DA
        TERM_REPLY(term, "\33[?6c");
        break;
        // SS2 and SS3 (N, O) have no G2 or G3 to shift to, so the next byte
        // prints as itself. The keypad modes (=, >) are not done yet.
    }
}

// The CSI sequences with no private marker and no intermediate.
static void term_csi_dispatch(term_state_t *term, uint8_t ch)
{
    switch (ch)
    {
    case '@':
        term_out_ICH(term);
        break;
    case 'A':
        term_cursor_up(term, term_param(term, 0, 1));
        break;
    case 'B':
    case 'e': // VPR -- alias of CUD
        term_cursor_down(term, term_param(term, 0, 1));
        break;
    case 'C':
    case 'a': // HPR -- alias of CUF
        term_cursor_forward(term, term_param(term, 0, 1));
        break;
    case 'D':
        term_cursor_back(term, term_param(term, 0, 1));
        break;
    case 'E': // CNL
        term_cursor_down(term, term_param(term, 0, 1));
        term_out_CR(term);
        break;
    case 'F': // CPL
        term_cursor_up(term, term_param(term, 0, 1));
        term_out_CR(term);
        break;
    case 'G':
    case '`': // HPA -- alias of CHA
        term_goto(term, term_param(term, 0, 1) - 1u, term->cur->y);
        break;
    case 'H':
    case 'f': // HVP -- alias of CUP
        term_out_CUP(term);
        break;
    case 'I': // CHT
        term_tab_forward(term, term_param(term, 0, 1));
        break;
    case 'J':
        term_erase_display(term, term->csi_param[0]);
        break;
    case 'K':
        term_erase_line(term, term->csi_param[0]);
        break;
    case 'L':
        term_out_IL_DL(term, true);
        break;
    case 'M':
        term_out_IL_DL(term, false);
        break;
    case 'P':
        term_out_DCH(term);
        break;
    case 'S':
        term_out_SU_SD(term, true);
        break;
    case 'T':
        term_out_SU_SD(term, false);
        break;
    case 'X':
        term_out_ECH(term);
        break;
    case 'Z': // CBT
        term_tab_back(term, term_param(term, 0, 1));
        break;
    case 'b':
        term_out_REP(term);
        break;
    case 'c': // Primary DA: a VT102
        if (!term->csi_param[0])
            TERM_REPLY(term, "\33[?6c");
        break;
    case 'd':
        term_out_VPA(term);
        break;
    case 'g':
        term_out_TBC(term);
        break;
    case 'h':
        term_out_SM_RM(term, true);
        break;
    case 'l':
        term_out_SM_RM(term, false);
        break;
    case 'm':
        term_out_SGR(term);
        break;
    case 'n':
        term_out_DSR(term);
        break;
    case 'r':
        term_out_DECSTBM(term);
        break;
    case 's': // DECSC (xterm's SCOSC, the same without left and right margins)
        term_save_cursor(term);
        break;
    case 't':
        term_out_window(term);
        break;
    case 'u': // DECRC
        term_restore_cursor(term);
        break;
    case 'x':
        term_out_DECREQTPARM(term);
        break;
    case 'y': // DECTST: the power-up test is a reset; the loopbacks are ignored
        if (term->csi_param[0] == 2 && term->csi_param_count == 2 &&
            term->csi_param[1] == 1)
            term_out_RIS(term);
        break;
    }
    term->rep_valid = false;
}

static void term_csi_final(term_state_t *term, uint8_t ch)
{
    term->ansi_state = ansi_state_ground;
    if (term->csi_param_count < TERM_CSI_PARAM_MAX_LEN)
        term->csi_param_count++;
    if (term->csi_colon && ch != 'm')
        return; // ':' belongs to SGR alone
    uint8_t inter = term->csi_intermediate;
    switch (term->csi_private)
    {
    case 0:
        if (!inter)
            return term_csi_dispatch(term, ch);
        if (inter == ' ' && ch == 'q')
            term_out_DECSCUSR(term);
        else if (inter == '!' && ch == 'p')
            term_out_DECSTR(term);
        else if (inter == '$' && ch == 'p')
            term_out_DECRQM(term, false);
        break;
    case '?':
        if (!inter && (ch == 'h' || ch == 'l'))
            term_out_DECSET(term, ch == 'h');
        else if (inter == '$' && ch == 'p')
            term_out_DECRQM(term, true);
        break;
    case '>': // Secondary DA: type 0, firmware 1, ROM 0
        if (!inter && ch == 'c' && !term->csi_param[0])
            TERM_REPLY(term, "\33[>0;1;0c");
        break;
    case '=': // Tertiary DA: a unit ID of zeros
        if (!inter && ch == 'c' && !term->csi_param[0])
            TERM_REPLY(term, "\33P!|00000000\33\\");
        break;
    }
    term->rep_valid = false;
}

// A byte of a CSI sequence, after the introducer. Parameters are digits, ';'
// and ':' (0x30-0x3B); a private marker (0x3C-0x3F) counts only as the first
// byte; intermediates (0x20-0x2F) come after the parameters; the final byte
// (0x40-0x7E) runs it. Anything out of that order makes the sequence one to
// drop, through its final byte.
static void term_csi_byte(term_state_t *term, uint8_t ch)
{
    if (term_seq_control(term, ch))
        return;
    if (ch >= 0x40)
    {
        if (term->ansi_state == ansi_state_csi_ignore || term->csi_intermediate == 0xFF)
        {
            // Malformed, or doubled intermediates: nothing we know.
            term->ansi_state = ansi_state_ground;
            term->rep_valid = false;
        }
        else
            term_csi_final(term, ch);
        return;
    }
    if (term->ansi_state == ansi_state_csi_ignore)
        return;
    if (ch < 0x30)
    {
        term->csi_intermediate = term->csi_intermediate ? 0xFF : ch;
        return;
    }
    if (term->csi_intermediate)
    {
        term->ansi_state = ansi_state_csi_ignore; // a parameter after an intermediate
        return;
    }
    uint8_t i = term->csi_param_count;
    if (ch >= 0x3C)
    {
        if (term->csi_any)
            term->ansi_state = ansi_state_csi_ignore;
        else
            term->csi_private = ch;
        term->csi_any = true;
        return;
    }
    term->csi_any = true;
    if (ch == ';' || ch == ':')
    {
        if (ch == ':')
            term->csi_colon = true;
        if (i < TERM_CSI_PARAM_MAX_LEN)
        {
            term->csi_separator[i] = (char)ch;
            term->csi_param_count = ++i;
            if (i < TERM_CSI_PARAM_MAX_LEN)
            {
                term->csi_param[i] = 0;
                term->csi_separator[i] = 0;
            }
        }
        return;
    }
    if (i < TERM_CSI_PARAM_MAX_LEN)
    {
        uint32_t v = term->csi_param[i] * 10u + (ch - '0');
        term->csi_param[i] = v > 0xFFFF ? 0xFFFF : (uint16_t)v;
    }
}

/* ---- VT52 mode --------------------------------------------------------- */

// ESC and a letter in VT52 mode, as a VT102 takes them there.
static void term_vt52_byte(term_state_t *term, uint8_t ch)
{
    if (term_seq_control(term, ch))
        return;
    term->ansi_state = ansi_state_ground;
    term->rep_valid = false;
    switch (ch)
    {
    case 'A': // cursor up
        term_cursor_up(term, 1);
        break;
    case 'B': // cursor down
        term_cursor_down(term, 1);
        break;
    case 'C': // cursor right
        term_cursor_forward(term, 1);
        break;
    case 'D': // cursor left
        term_cursor_back(term, 1);
        break;
    case 'F': // graphics mode: DEC Special Graphics, as the VT102 and xterm do
        term->cur->g0_charset = TERM_CS_DEC;
        term->cur->gl_is_g1 = false;
        break;
    case 'G': // ASCII mode
        term->cur->g0_charset = TERM_CS_ASCII;
        term->cur->gl_is_g1 = false;
        break;
    case 'H': // home
        term_goto(term, 0, 0);
        break;
    case 'I': // reverse line feed
        term_reverse_index(term);
        break;
    case 'J': // erase to end of screen
        term_erase_display(term, 0);
        break;
    case 'K': // erase to end of line
        term_erase_line(term, 0);
        break;
    case 'Y': // direct cursor address: row and column, each plus 32
        term->ansi_state = ansi_state_vt52_row;
        break;
    case 'Z': // identify: a VT100 in VT52 mode
        TERM_REPLY(term, "\33/Z");
        break;
    case '<': // ANSI mode
        term->vt52 = false;
        break;
        // The keypad modes (=, >) are not done yet.
    }
}

// The row and column of ESC Y. A row off the screen leaves the cursor's row
// alone; a column off it goes to the last column.
static void term_vt52_address(term_state_t *term, uint8_t ch)
{
    if (term_seq_control(term, ch))
        return;
    if (term->ansi_state == ansi_state_vt52_row)
    {
        term->csi_param[0] = ch;
        term->ansi_state = ansi_state_vt52_col;
        return;
    }
    term->ansi_state = ansi_state_ground;
    term->rep_valid = false;
    unsigned row = (unsigned)(term->csi_param[0] - 32);
    unsigned col = (unsigned)(ch - 32);
    if (row >= term->height)
        row = term->cur->y;
    term_goto(term, col, row);
}

static void term_out_char(term_state_t *term, uint8_t ch)
{
    ansi_state_t state = term->ansi_state;
    if (ch == TERM_C0_ESC)
    {
        // ESC starts a new sequence from any state; in an OSC it may be ST.
        term->osc_pending = (state == ansi_state_osc);
        term->ansi_state = term->vt52 ? ansi_state_vt52_esc : ansi_state_esc;
        return;
    }
    if (state != ansi_state_ground && (ch == TERM_C0_CAN || ch == TERM_C0_SUB))
    {
        // CAN and SUB abort a sequence or a string, silently. An OSC's items
        // apply as they arrive, so the pens take what an aborted one set.
        term->ansi_state = ansi_state_ground;
        term->rep_valid = false;
        if (state == ansi_state_osc || state == ansi_state_osc_palette)
            term_resolve_all_pens(term);
        return;
    }
    switch (state)
    {
    case ansi_state_ground:
        term_ground(term, ch);
        break;
    case ansi_state_esc:
        term_esc_byte(term, ch);
        break;
    case ansi_state_esc_int:
        term_esc_int_byte(term, ch);
        break;
    case ansi_state_csi:
    case ansi_state_csi_ignore:
        term_csi_byte(term, ch);
        break;
    case ansi_state_osc:
        term_osc_byte(term, ch);
        break;
    case ansi_state_osc_palette:
        term_osc_palette_byte(term, ch);
        break;
    case ansi_state_string:
        break;
    case ansi_state_vt52_esc:
        term_vt52_byte(term, ch);
        break;
    case ansi_state_vt52_row:
    case ansi_state_vt52_col:
        term_vt52_address(term, ch);
        break;
    case ansi_state_count:
        break;
    }
}

static void term_out_chars(const char *buf, int length)
{
    // Snapshot the cursor before each char; only chars whose processing
    // actually moves it trip the input gap. Per-term so a canvas swap leaves
    // both blink schedules consistent.
    for (int i = 0; i < length; i++)
    {
        term_state_t *terms[2] = {&term_40, &term_80};
        for (int t = 0; t < 2; t++)
        {
            term_state_t *term = terms[t];
            uint8_t px = term->cur->x, py = term->cur->y;
            bool pw = term->cur->wrap_pending;
            term_out_char(term, (uint8_t)buf[i]);
            if (term->cur->x != px || term->cur->y != py || term->cur->wrap_pending != pw)
                term_cursor_restart_blink(term);
        }
    }
}

void term_init(void)
{
    // prepare console
    memcpy(color_256_term, color_256, sizeof(color_256_term));
    static term_data_t term40_pri_mem[40 * TERM_MAX_HEIGHT];
    static term_data_t term80_pri_mem[80 * TERM_MAX_HEIGHT];
#if TERM_ALT_SCREEN
    static term_data_t term40_alt_mem[40 * TERM_MAX_HEIGHT];
    static term_data_t term80_alt_mem[80 * TERM_MAX_HEIGHT];
    term_state_init(&term_40, 40, term40_pri_mem, term40_alt_mem);
    term_state_init(&term_80, 80, term80_pri_mem, term80_alt_mem);
#else
    /* No alt store, so bufs[1] carries state but never cells. Nothing
     * may swap to it or mark it erased; see term_enter_alt. */
    term_state_init(&term_40, 40, term40_pri_mem, NULL);
    term_state_init(&term_80, 80, term80_pri_mem, NULL);
#endif
    // become part of stdout
    manifold_set_term_out(term_out_chars);
}

// Cursor blink timing only — toggles cursor_lit on the configured cadence
// (fast while a wrap is pending, normal otherwise) and forces unlit when
// the cursor is disabled. Steady-vs-blink handling lives in the renderers,
// which ignore cursor_lit when cursor_style is steady; this keeps the
// timing path style-agnostic.
static void term_blink_cursor(term_state_t *term)
{
    if (!term->cursor_enabled)
    {
        term->cursor_lit = false;
        return;
    }
    if ((long)(vga_frame_count() - term->cursor_frame) < 0)
        return;
    term->cursor_lit = !term->cursor_lit;
    term->cursor_frame = vga_frame_count() + (term->cur->wrap_pending
                                                  ? TERM_CURSOR_BLINK_FAST_FRAMES
                                                  : TERM_CURSOR_BLINK_FRAMES);
}

// SGR-5/6 cell blink phase, per-term: a free-running 2-bit counter at bits 0-1
// (TERM_ATTR_ANY_BLINK). Bit 0 (TERM_ATTR_BLINK_FAST) flips every tick -> SGR 6 rapid;
// bit 1 (TERM_ATTR_BLINK) every two ticks -> SGR 5 slow. The renderer ANDs a cell's
// single blink bit against this value, so each cell darkens only on its own
// rate's off-phase, and both relight together at phase 0. Incremented from
// Core 0 here; read by Core 1 in the renderer hot path. Per-term so the cursor
// timer and cell blink timer share an owner instead of crossing a global.
static void term_cell_blink_phase_task(term_state_t *term)
{
    if ((long)(vga_frame_count() - term->cell_blink_frame) >= 0)
    {
        term->cell_blink_phase =
            (uint8_t)((term->cell_blink_phase + 1) & TERM_ATTR_ANY_BLINK);
        term->cell_blink_frame = vga_frame_count() + TERM_BLINK_TICK_FRAMES;
    }
}

void term_task(void)
{
    term_blink_cursor(&term_40);
    term_blink_cursor(&term_80);
    term_cell_blink_phase_task(&term_40);
    term_cell_blink_phase_task(&term_80);
    term_clean_task(&term_40);
    term_clean_task(&term_80);
}

void term_RIS(void)
{
    term_out_RIS(&term_40);
    term_out_RIS(&term_80);
}

void term_RIS_no_clear(void)
{
    term_out_RIS_no_clear(&term_40);
    term_out_RIS_no_clear(&term_80);
}

/* ---- savestate ----------------------------------------------------------
 *
 * A terminal is two screens of cells, a cursor state and a saved cursor for
 * each, and the parser's own place in whatever escape sequence was arriving
 * when the blob was taken.
 *
 * Written field by field. cursor_state_t has padding, and a struct copy
 * would put whatever is in it into the blob, where it would make two saves of
 * one unchanged machine differ. */

static void term_put_cursor(sst_cursor_t *c, const cursor_state_t *cs)
{
    sst_put_u8(c, cs->x);
    sst_put_u8(c, cs->y);
    sst_put_bool(c, cs->wrap_pending);
    sst_put_u8(c, cs->sgr_attr);
    sst_put_u8(c, cs->fg_kind);
    sst_put_u8(c, cs->fg_index);
    sst_put_u8(c, cs->bg_kind);
    sst_put_u8(c, cs->bg_index);
    sst_put_u16(c, cs->fg_rgb);
    sst_put_u16(c, cs->bg_rgb);
    sst_put_u16(c, cs->fg_color);
    sst_put_u16(c, cs->bg_color);
    sst_put_u16(c, cs->ul_color);
    sst_put_bool(c, cs->underline_color_set);
    sst_put_bool(c, cs->bold);
    sst_put_bool(c, cs->faint);
    sst_put_bool(c, cs->reverse);
    sst_put_bool(c, cs->conceal);
    sst_put_bool(c, cs->origin_mode);
    sst_put_u8(c, cs->g0_charset);
    sst_put_u8(c, cs->g1_charset);
    sst_put_bool(c, cs->gl_is_g1);
}

static bool term_get_cursor(sst_cursor_t *c, cursor_state_t *cs, uint8_t width)
{
    cs->x = sst_get_u8(c);
    cs->y = sst_get_u8(c);
    cs->wrap_pending = sst_get_bool(c);
    cs->sgr_attr = sst_get_u8(c);
    cs->fg_kind = sst_get_u8(c);
    cs->fg_index = sst_get_u8(c);
    cs->bg_kind = sst_get_u8(c);
    cs->bg_index = sst_get_u8(c);
    cs->fg_rgb = sst_get_u16(c);
    cs->bg_rgb = sst_get_u16(c);
    cs->fg_color = sst_get_u16(c);
    cs->bg_color = sst_get_u16(c);
    cs->ul_color = sst_get_u16(c);
    cs->underline_color_set = sst_get_bool(c);
    cs->bold = sst_get_bool(c);
    cs->faint = sst_get_bool(c);
    cs->reverse = sst_get_bool(c);
    cs->conceal = sst_get_bool(c);
    cs->origin_mode = sst_get_bool(c);
    cs->g0_charset = sst_get_u8(c);
    cs->g1_charset = sst_get_u8(c);
    cs->gl_is_g1 = sst_get_bool(c);
    /* The row and column index the cells. */
    return cs->x < width && cs->y < TERM_MAX_HEIGHT;
}

static void term_put_screen(sst_cursor_t *c, const screen_buf_t *b)
{
    term_put_cursor(c, &b->cs);
    term_put_cursor(c, &b->saved);
    sst_put_bool(c, b->has_saved);
    for (int i = 0; i < TERM_MAX_HEIGHT; i++)
        sst_put_u8(c, b->row_idx[i]);
    for (int i = 0; i < TERM_MAX_HEIGHT; i++)
        sst_put_bool(c, b->dirty[i]);
    for (int i = 0; i < TERM_MAX_HEIGHT; i++)
        sst_put_u16(c, b->erase_fg_color[i]);
    for (int i = 0; i < TERM_MAX_HEIGHT; i++)
        sst_put_u16(c, b->erase_bg_color[i]);
    sst_put_u8(c, b->y_offset);
    sst_put_u8(c, b->hidden);
    sst_put_bool(c, b->all_clean);
}

static bool term_get_screen(sst_cursor_t *c, screen_buf_t *b, uint8_t width)
{
    bool ok = term_get_cursor(c, &b->cs, width);
    ok &= term_get_cursor(c, &b->saved, width);
    b->has_saved = sst_get_bool(c);
    for (int i = 0; i < TERM_MAX_HEIGHT; i++)
        b->row_idx[i] = sst_get_u8(c);
    for (int i = 0; i < TERM_MAX_HEIGHT; i++)
        b->dirty[i] = sst_get_bool(c);
    for (int i = 0; i < TERM_MAX_HEIGHT; i++)
        b->erase_fg_color[i] = sst_get_u16(c);
    for (int i = 0; i < TERM_MAX_HEIGHT; i++)
        b->erase_bg_color[i] = sst_get_u16(c);
    b->y_offset = sst_get_u8(c);
    b->hidden = sst_get_u8(c);
    b->all_clean = sst_get_bool(c);
    /* Everything here indexes the cell array. row_idx is the scroll remap and
     * y_offset is added to it before the lookup, so both are bounded. */
    if (!ok || !sst_ok(c) || b->y_offset >= TERM_MAX_HEIGHT || b->hidden >= TERM_MAX_HEIGHT)
        return false;
    for (int i = 0; i < TERM_MAX_HEIGHT; i++)
        if (b->row_idx[i] >= TERM_MAX_HEIGHT)
            return false;
    return true;
}

static void term_put_one(sst_cursor_t *c, const term_state_t *t)
{
    sst_put_u8(c, t->width);
    sst_put_u8(c, t->height);
    sst_put_bool(c, t->alt_active);
    sst_put_u8(c, t->margin_top);
    sst_put_u8(c, t->margin_bot);
    sst_put_bool(c, t->autowrap);
    sst_put_bool(c, t->insert_mode);
    sst_put_bool(c, t->newline_mode);
    sst_put_bool(c, t->disp_ctrl);
    sst_put_bool(c, t->vt52);
    sst_put_bool(c, t->ice_colors);
    sst_put_bool(c, t->cursor_enabled);
    sst_put_u8(c, t->cursor_style);
    sst_put_u64(c, t->cursor_frame);
    sst_put_bool(c, t->cursor_lit);
    sst_put_u64(c, t->cell_blink_frame);
    sst_put_u8(c, t->cell_blink_phase);
    sst_put_u16(c, t->default_fg_color);
    sst_put_u16(c, t->default_bg_color);
    sst_put_u16(c, t->cursor_color);
    sst_put_u8(c, t->last_glyph);
    sst_put_bool(c, t->rep_valid);
    sst_put(c, t->tab_stops, TERM_TAB_BITMAP_BYTES);
    sst_put_u8(c, (uint8_t)t->ansi_state);
    for (int i = 0; i < TERM_CSI_PARAM_MAX_LEN; i++)
        sst_put_u16(c, t->csi_param[i]);
    for (int i = 0; i < TERM_CSI_PARAM_MAX_LEN; i++)
        sst_put_u8(c, (uint8_t)t->csi_separator[i]);
    sst_put_u8(c, t->csi_param_count);
    sst_put_u8(c, t->csi_private);
    sst_put_u8(c, t->csi_intermediate);
    sst_put_bool(c, t->csi_colon);
    sst_put_bool(c, t->csi_any);
    sst_put_bool(c, t->osc_pending);
    sst_put_u16(c, t->osc_ps);
    sst_put_u8(c, t->osc_stage);
    sst_put_u8(c, t->osc_item);
    sst_put_u16(c, t->osc_index);
    sst_put_u8(c, t->osc_len);
    sst_put(c, t->osc_spec, TERM_OSC_SPEC_MAX);
    term_put_screen(c, &t->bufs[0]);
    term_put_screen(c, &t->bufs[1]);
}

static bool term_get_one(sst_cursor_t *c, term_state_t *t, uint8_t width)
{
    uint8_t w = sst_get_u8(c);
    uint8_t h = sst_get_u8(c);
    bool alt = sst_get_bool(c);
    t->margin_top = sst_get_u8(c);
    t->margin_bot = sst_get_u8(c);
    t->autowrap = sst_get_bool(c);
    t->insert_mode = sst_get_bool(c);
    t->newline_mode = sst_get_bool(c);
    t->disp_ctrl = sst_get_bool(c);
    t->vt52 = sst_get_bool(c);
    t->ice_colors = sst_get_bool(c);
    t->cursor_enabled = sst_get_bool(c);
    t->cursor_style = sst_get_u8(c);
    t->cursor_frame = (unsigned long)sst_get_u64(c);
    t->cursor_lit = sst_get_bool(c);
    t->cell_blink_frame = (unsigned long)sst_get_u64(c);
    t->cell_blink_phase = sst_get_u8(c);
    t->default_fg_color = sst_get_u16(c);
    t->default_bg_color = sst_get_u16(c);
    t->cursor_color = sst_get_u16(c);
    t->last_glyph = sst_get_u8(c);
    t->rep_valid = sst_get_bool(c);
    sst_get(c, t->tab_stops, TERM_TAB_BITMAP_BYTES);
    uint8_t state = sst_get_u8(c);
    for (int i = 0; i < TERM_CSI_PARAM_MAX_LEN; i++)
        t->csi_param[i] = sst_get_u16(c);
    for (int i = 0; i < TERM_CSI_PARAM_MAX_LEN; i++)
        t->csi_separator[i] = (char)sst_get_u8(c);
    t->csi_param_count = sst_get_u8(c);
    t->csi_private = sst_get_u8(c);
    t->csi_intermediate = sst_get_u8(c);
    t->csi_colon = sst_get_bool(c);
    t->csi_any = sst_get_bool(c);
    t->osc_pending = sst_get_bool(c);
    t->osc_ps = sst_get_u16(c);
    t->osc_stage = sst_get_u8(c);
    t->osc_item = sst_get_u8(c);
    t->osc_index = sst_get_u16(c);
    t->osc_len = sst_get_u8(c);
    sst_get(c, t->osc_spec, TERM_OSC_SPEC_MAX);
    if (!term_get_screen(c, &t->bufs[0], width) || !term_get_screen(c, &t->bufs[1], width))
        return false;
    /* A terminal is a different object at another width or height, and the
     * alt screen only exists in a build that has one. Everything after that
     * is a bound on something that indexes: the margins and the cursor rows
     * the cells, the parser counts their arrays, and in the Linux palette
     * sequence osc_len the spec it is filling. */
    if (!sst_ok(c) || w != width || h < 1 || h > TERM_MAX_HEIGHT)
        return false;
    if (alt && !t->bufs[1].mem)
        return false;
    if (state >= ansi_state_count || t->margin_top > t->margin_bot || t->margin_bot >= h ||
        t->csi_param_count > TERM_CSI_PARAM_MAX_LEN || t->osc_len > TERM_OSC_SPEC_MAX + 1 ||
        (state == ansi_state_osc_palette && t->osc_len >= 7))
        return false;
    t->ansi_state = (ansi_state_t)state;
    t->height = h;
    t->alt_active = alt;
    t->screen = &t->bufs[alt ? 1 : 0];
    t->cur = &t->screen->cs;
    return t->cur->y < h;
}

void term_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    sst_put_u8(c, TERM_MAX_HEIGHT);
    sst_put_bool(c, TERM_ALT_SCREEN ? true : false);
    sst_put_u8(c, 2); /* how many terminals this machine has */
    term_put_one(c, &term_40);
    term_put_one(c, &term_80);
    for (int b = 0; b < (TERM_ALT_SCREEN ? 2 : 1); b++)
    {
        sst_put(c, term_40.bufs[b].mem, 40 * TERM_MAX_HEIGHT * sizeof(term_data_t));
        sst_put(c, term_80.bufs[b].mem, 80 * TERM_MAX_HEIGHT * sizeof(term_data_t));
    }
    sst_put(c, color_256_term, sizeof color_256_term);
}

bool term_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    uint8_t rows = sst_get_u8(c);
    bool alt = sst_get_bool(c);
    uint8_t count = sst_get_u8(c);
    if (!sst_ok(c) || rows != TERM_MAX_HEIGHT ||
        alt != (TERM_ALT_SCREEN ? true : false) || count != 2)
        return false;
    if (!term_get_one(c, &term_40, 40) || !term_get_one(c, &term_80, 80))
        return false;
    /* Never mem: the cell arrays are term_init's own statics and their
     * addresses are already right. Only what is in them is the blob's. */
    for (int b = 0; b < (TERM_ALT_SCREEN ? 2 : 1); b++)
    {
        sst_get(c, term_40.bufs[b].mem, 40 * TERM_MAX_HEIGHT * sizeof(term_data_t));
        sst_get(c, term_80.bufs[b].mem, 80 * TERM_MAX_HEIGHT * sizeof(term_data_t));
    }
    sst_get(c, color_256_term, sizeof color_256_term);
    return sst_ok(c);
}

static term_state_t *term_visible(void)
{
    return vga_canvas_width() == 320 ? &term_40 : &term_80;
}

void term_view(term_view_t *out)
{
    term_state_t *term = term_visible();
    out->width = term->width;
    out->height = term->height;
    // A pending wrap is shown as the column past the last, which is how the
    // view knows to draw it.
    out->cursor_x = (uint8_t)(term->cur->x + (term->cur->wrap_pending ? 1 : 0));
    out->cursor_y = term->cur->y;
    out->cursor_style = term->cursor_style;
    out->cursor_enabled = term->cursor_enabled;
    out->cursor_lit = term->cursor_lit;
    out->cursor_color = term->cursor_color;
    out->blink_phase = term->cell_blink_phase;
}

const term_data_t *term_view_row(uint8_t y)
{
    return term_row_ptr(term_visible(), y);
}

void term_set_height(uint8_t width, uint8_t height)
{
    term_state_set_height(width == 40 ? &term_40 : &term_80, height);
}
