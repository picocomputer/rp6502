/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "modes/modes.h"
#include "term/color.h"
#include "term/font.h"
#include "term/term.h"
#include "sys/com.h"
#include "sys/vga.h"
#include "scanvideo/scanvideo.h"
#include <pico/stdlib.h>
#include <pico/stdio/driver.h>
#include <stdio.h>
#include <string.h>

// Implements the Linux console subset of ECMA-48 / VT102 with
// xterm-color (256-color, truecolor, OSC 10/11/12) extensions.
// See `man 4 console_codes`. DEC Special Graphics charset
// (ESC ( 0 / SO / SI) is supported via an attribute bit plus a
// parallel font buffer sourced from CP437. Alt screen buffer:
// ?1047/?1049 lazy-clear on entry (rows marked dirty, drained by the
// background term_clean_task); ?47 swaps without clearing (content kept).
// 8-bit codepage encoding only -- no UTF-8 decode.
//
// Designed to keep up with 115200 bps without flow control.
// The logic herein will make more sense if you remember this:

// 1. The screen data doesn't move when scrolling. Instead, the
//    video begins rendering at y_offset and wraps around.
// 2. The screen doesn't fully clear immediately. To keep the UART
//    buffer from overflowing, lines are cleared in a background task
//    and checked as the cursor moves into them.
// 3. Each cell's `attributes` byte holds render-time flags
//    (underline, blink, DEC graphics, etc.). The renderer's hot
//    path branches once on `attr != 0` per cell; the common path
//    pays only a 1-byte load and a predicted-not-taken branch.

#define TERM_STD_HEIGHT 30
#define TERM_MAX_HEIGHT 32
#define TERM_MAX_WIDTH 80
#define TERM_TAB_BITMAP_BYTES ((TERM_MAX_WIDTH + 7) / 8)
#define TERM_CSI_PARAM_MAX_LEN 16
#define TERM_FG_COLOR_INDEX 7
#define TERM_BG_COLOR_INDEX 0

// Cell `attributes` byte: render-time flags.
#define ATTR_BLINK_FAST (1u << 0) // SGR 6 rapid (blink phase bit 0)
#define ATTR_BLINK (1u << 1)      // SGR 5 slow  (blink phase bit 1)
#define ATTR_UNDERLINE (1u << 2)  // SGR 4
#define ATTR_DBL_UL (1u << 3)     // SGR 21
#define ATTR_STRIKE (1u << 4)     // SGR 9
#define ATTR_OVERLINE (1u << 5)   // SGR 53
#define ATTR_DEC (1u << 6)        // cell renders from DEC graphics buffer
#define ATTR_ITALIC (1u << 7)     // SGR 3 (ASCII-only, 8x16 mode only)
// SGR 5 (slow) and SGR 6 (rapid) live at bits 0-1 so they map onto the live
// blink phase counter directly. A cell carries exactly one of the two; the
// renderer ANDs it against the phase so each pulses at its own rate. The two
// underlines sit adjacent at bits 2-3. See TERM_BLINK_TICK_US.
#define ATTR_ANY_BLINK (ATTR_BLINK | ATTR_BLINK_FAST)
#define ATTR_RENDER_MASK (ATTR_BLINK | ATTR_BLINK_FAST | ATTR_UNDERLINE | \
                          ATTR_DBL_UL | ATTR_STRIKE | ATTR_OVERLINE |     \
                          ATTR_DEC | ATTR_ITALIC)

// SGR-state-only flags (not stored per-cell): emit-time fg/bg transforms.
// Held in cursor_state_t alongside bold/faint, not in sgr_attr.

// Cell-blink base tick = the SGR 6 rapid-blink half-period. The phase counter
// (cell_blink_phase) increments every tick: bit 0 flips each tick (SGR 6 rapid,
// ~3 Hz), bit 1 every two ticks (SGR 5 slow, ~1.5 Hz -- the original rate).
// ~3 Hz clears ECMA-48's 150/min (2.5 Hz) slow/rapid boundary. Slightly off
// 166.67ms to avoid frame-tear sync with display refresh.
#define TERM_BLINK_TICK_US 166500

// Cursor blink half-period: normal cell vs "stuck at right edge"
// (off-screen deferred-wrap state blinks at 2x rate so it's still noticeable).
// 0.3ms drift to avoid blinking cursor tearing.
#define TERM_CURSOR_BLINK_US 499700
#define TERM_CURSOR_BLINK_FAST_US 249700

// Whenever input changes the cursor's logical position or its
// appearance (DECSCUSR style, DECTCEM visibility), the cursor is
// forced unlit and its next blink tick is armed this far out. Pure
// SGR/OSC sequences and color-only changes (OSC 12/112) leave the
// blink schedule untouched. The gap re-lights to the appropriate
// steady/blink state when it fires.
#define TERM_CURSOR_INPUT_GAP_US 5000

typedef enum
{
    ansi_state_C0,
    ansi_state_Fe,
    ansi_state_SS2,
    ansi_state_SS3,
    ansi_state_CSI,
    ansi_state_CSI_less,
    ansi_state_CSI_equal,
    ansi_state_CSI_greater,
    ansi_state_CSI_question,
    ansi_state_OSC,
    ansi_state_OSC_esc,
    ansi_state_Fe_hash,     // after ESC #
    ansi_state_Fe_paren_G0, // after ESC ( (designate G0)
    ansi_state_Fe_paren_G1, // after ESC ) (designate G1)
} ansi_state_t;

typedef struct
{
    uint8_t font_code;
    uint8_t attributes;
    uint16_t fg_color;
    uint16_t bg_color;
    uint16_t ul_color; // resolved at cell-write: SGR 58 color, or fg if not set
} term_data_t;

_Static_assert(sizeof(term_data_t) == 8, "term_data_t size lock for cell-memory budget");

// Cursor + SGR + DECSC-saved mode state. Per-screen (lives inside
// screen_buf_t), and also reused as the storage type for DECSC and ?1049
// snapshots so the field list is defined once.
// fg_color_index sentinel: fg was set via SGR 38 (256-color or RGB) and the
// recompute path should pull from user_fg_color instead of color_256[].
// bg_color_index sentinel: bg was set via SGR 48 or SGR 100-107; the
// recompute path should pull from user_bg_color. (SGR 90-97 doesn't need a
// sentinel — it stores the bright slot 8..15 directly, which color_256[]
// resolves correctly. The bg side can't do that because ice_colors mode
// reads bg_color_index + 8.)
#define FG_COLOR_INDEX_EXTENDED 0xFF
#define BG_COLOR_INDEX_EXTENDED 0xFF

typedef struct
{
    uint8_t x, y;
    uint8_t sgr_attr;
    uint8_t fg_color_index, bg_color_index;
    uint16_t fg_color, bg_color;
    uint16_t user_fg_color;   // base color from SGR 38, used when fg_color_index == FG_COLOR_INDEX_EXTENDED
    uint16_t user_bg_color;   // base color from SGR 48 / 100-107, used when bg_color_index == BG_COLOR_INDEX_EXTENDED
    uint16_t ul_color;        // SGR 58, valid only when underline_color_set
    bool underline_color_set; // SGR 58 active; SGR 59 / SGR 0 clear it
    bool bold;
    bool faint;
    bool reverse; // SGR 7: emit-time fg/bg swap
    bool conceal; // SGR 8: emit-time fg = bg
    bool origin_mode;
    bool line_wrap;
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
    uint8_t row_idx[TERM_MAX_HEIGHT];
    bool dirty[TERM_MAX_HEIGHT];
    uint16_t erase_fg_color[TERM_MAX_HEIGHT];
    uint16_t erase_bg_color[TERM_MAX_HEIGHT];
    uint16_t erase_ul_color[TERM_MAX_HEIGHT];
    uint8_t erase_attr[TERM_MAX_HEIGHT];
    uint8_t y_offset;
    bool all_clean;
    uint8_t margin_top, margin_bot;
} screen_buf_t;

typedef struct term_state
{
    uint8_t width;
    uint8_t height;
    /* Per-screen state. cur is &screen->cs, swapped with screen on alt
     * enter/exit. All cursor + SGR access goes through cur. */
    screen_buf_t bufs[2];       /* [0] = primary, [1] = alt */
    screen_buf_t *screen;       /* active buffer */
    cursor_state_t *cur;        /* = &screen->cs */
    bool alt_active;            /* true when screen == &bufs[1] */
    cursor_state_t cursor_save; /* ?1049 snapshot */
    bool cursor_save_valid;
    /* SCO-style save (CSI s / u) -- separate from DECSC and ?1049, and (like
     * them) stored at term level, so it persists across an alt-screen swap
     * rather than being per-screen as in xterm. */
    uint8_t save_x;
    uint8_t save_y;
    bool save_origin_mode;
    bool cursor_enabled;
    absolute_time_t cursor_timer;
    volatile bool cursor_lit;
    absolute_time_t cell_blink_timer;
    volatile uint8_t cell_blink_phase;
    uint16_t default_fg_color;
    uint16_t default_bg_color;
    uint16_t cursor_color; // OSC 12
    term_data_t *ptr;
    ansi_state_t ansi_state;
    uint16_t csi_param[TERM_CSI_PARAM_MAX_LEN];
    char csi_separator[TERM_CSI_PARAM_MAX_LEN];
    uint8_t csi_param_count;
    uint8_t csi_intermediate;                 // ' ', '!', or '$' captured during CSI parse
    bool ice_colors;                          // ?33 -- SGR 5/6 means bright bg (the IBM VGA hack), not blink
    uint8_t cursor_style;                     // DECSCUSR Ps: 0/1/2=block, 3/4=underline, 5/6=bar (read by term_render_*)
    uint8_t tab_stops[TERM_TAB_BITMAP_BYTES]; // 1 bit per column
    cursor_state_t decsc;                     // DECSC snapshot (ESC 7 / ESC 8)
    bool decsc_valid;
} term_state_t;

static term_state_t term_40;
static term_state_t term_80;
static int16_t term_scanline_begin;

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
// while a region scroll is mid-update — same severity as today's
// y_offset++ race. No memory barrier required.
static inline term_data_t *term_row_ptr(const term_state_t *term, uint8_t y)
{
    uint8_t slot = term_buf_slot(term->screen->y_offset, y);
    return term->screen->mem + (uint32_t)term->screen->row_idx[slot] * term->width;
}

// Compute the effective fg/bg/attr a cell write should land with, given
// the current SGR state. Applies emit-time REVERSE/CONCEAL toggles; render
// bits (UL/STRIKE/OVERLINE/DBL_UL/BLINK/BLINK_FAST) flow through unchanged.
// ATTR_DEC is the caller's responsibility -- only set for DEC glyph cells.
// In ice_colors mode (DECSET ?33) both blink bits (ATTR_ANY_BLINK) are
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
    uint8_t cell_mask = ATTR_RENDER_MASK & ~ATTR_DEC;
    if (term->ice_colors)
        cell_mask &= (uint8_t)~ATTR_ANY_BLINK;
    *attr = (uint8_t)(term->cur->sgr_attr & cell_mask);
}

// term_emit_attrs plus the SGR-58 underline-color fallback (ul_color when
// SGR 58 is active, else fg). Single source of truth for the fg/bg/ul/attr an
// erase or clear should land with; used by every lazy-erase / clear path.
static inline void term_emit_erase(const term_state_t *term, uint16_t *fg,
                                   uint16_t *bg, uint16_t *ul, uint8_t *attr)
{
    term_emit_attrs(term, fg, bg, attr);
    *ul = term->cur->underline_color_set ? term->cur->ul_color : *fg;
}

// Paint [start, end) of `row` with space glyphs at the given fg/bg/attr.
// Used by every clear/erase/insert path; keep this the only place that
// touches all five cell fields together.
static inline void fill_cells(term_data_t *row, unsigned start, unsigned end,
                              uint16_t fg, uint16_t bg, uint16_t ul, uint8_t attr)
{
    for (unsigned i = start; i < end; i++)
    {
        row[i].font_code = ' ';
        row[i].fg_color = fg;
        row[i].bg_color = bg;
        row[i].ul_color = ul;
        row[i].attributes = attr;
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
               term->screen->erase_bg_color[y],
               term->screen->erase_ul_color[y],
               term->screen->erase_attr[y]);
}

// Force the cursor unlit and arm the input-gap timer. Used by movement
// (com_out_chars) and by appearance changes (DECSCUSR, DECTCEM enable)
// so the cursor's new state lands on a clean blink boundary.
static inline void term_cursor_restart_blink(term_state_t *term)
{
    term->cursor_lit = false;
    term->cursor_timer = make_timeout_time_us(TERM_CURSOR_INPUT_GAP_US);
}

// Refresh term->ptr to track cur->{x,y} after row_idx[] or y_offset has
// moved, and clean the cursor's row so a follow-up glyph write lands on
// fresh memory. Tail of every routine that scrolls or moves rows.
static inline void term_refresh_cursor_ptr(term_state_t *term)
{
    term->ptr = term_row_ptr(term, term->cur->y) + term->cur->x;
    term_clean_line(term, term->cur->y);
}

// Set a new cursor position, 0-indexed. x == term->width is legal and
// places the cursor in the deferred-wrap parked state (one past row end);
// CUP / RCP / DECRC / wrap-on glyph emit all route through here.
static void term_set_cursor_position(term_state_t *term, uint16_t x, uint16_t y)
{
    bool x_off_screen = false;
    if (x == term->width)
    {
        x--;
        x_off_screen = true;
    }
    term->cur->x = x;
    term->cur->y = y;
    term->ptr = term_row_ptr(term, y) + x;
    term_clean_line(term, y);
    if (x_off_screen)
    {
        // ptr parks one column past row end; matches "pending wrap" state.
        term->cur->x++;
        term->ptr++;
    }
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
            fill_cells(row, 0, width,
                       buf->erase_fg_color[i],
                       buf->erase_bg_color[i],
                       buf->erase_ul_color[i],
                       buf->erase_attr[i]);
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
// so the cursor cell is never affected by this task and no unlit dance
// is needed.
static void term_clean_task(term_state_t *term)
{
    if (term_clean_one_dirty(term->screen, term->width, term->height))
        return;
    screen_buf_t *other = &term->bufs[term->alt_active ? 0 : 1];
    term_clean_one_dirty(other, term->width, term->height);
}

static void term_shift_meta_up(term_state_t *term, uint8_t count)
{
    for (uint8_t i = 0; i < count; i++)
    {
        term->screen->dirty[i] = term->screen->dirty[i + 1];
        term->screen->erase_fg_color[i] = term->screen->erase_fg_color[i + 1];
        term->screen->erase_bg_color[i] = term->screen->erase_bg_color[i + 1];
        term->screen->erase_ul_color[i] = term->screen->erase_ul_color[i + 1];
        term->screen->erase_attr[i] = term->screen->erase_attr[i + 1];
    }
}

static void term_shift_meta_down(term_state_t *term, uint8_t start)
{
    for (int i = start; i > 0; i--)
    {
        term->screen->dirty[i] = term->screen->dirty[i - 1];
        term->screen->erase_fg_color[i] = term->screen->erase_fg_color[i - 1];
        term->screen->erase_bg_color[i] = term->screen->erase_bg_color[i - 1];
        term->screen->erase_ul_color[i] = term->screen->erase_ul_color[i - 1];
        term->screen->erase_attr[i] = term->screen->erase_attr[i - 1];
    }
}

static void term_mark_rows_erase(term_state_t *term, uint8_t from, uint8_t to)
{
    uint16_t fg, bg, ul;
    uint8_t attr;
    term_emit_erase(term, &fg, &bg, &ul, &attr);
    for (uint8_t i = from; i < to; i++)
    {
        term->screen->dirty[i] = true;
        term->screen->erase_fg_color[i] = fg;
        term->screen->erase_bg_color[i] = bg;
        term->screen->erase_ul_color[i] = ul;
        term->screen->erase_attr[i] = attr;
    }
    term->screen->all_clean = false;
}

// Full screen clear + home cursor. Backs the C0 \f mapping; also used by
// RIS. The name reflects the work done, not the dispatch source.
static void term_full_clear(term_state_t *term)
{
    term_mark_rows_erase(term, 0, term->height);
    term->cur->x = 0;
    term->cur->y = 0;
    term->screen->y_offset = 0;
    // Rotation is meaningless after a full clear; restore identity so any
    // permutation left by a prior DECSTBM scroll doesn't outlive the wipe.
    for (uint8_t i = 0; i < TERM_MAX_HEIGHT; i++)
        term->screen->row_idx[i] = i;
    term->ptr = term_row_ptr(term, 0);
    term_clean_line(term, 0);
}

static void term_reset_tab_stops(term_state_t *term)
{
    memset(term->tab_stops, 0, sizeof(term->tab_stops));
    for (uint8_t col = 8; col < term->width; col += 8)
        term->tab_stops[col >> 3] |= 1u << (col & 7);
}

// Reset SGR + mode state that both RIS (hard) and DECSTR (soft) reset.
// Does NOT touch: screen contents, tab stops, default colors, alt-screen
// state, ansi parser state, or the cursor's on-screen position.
static void term_reset_sgr_and_modes(term_state_t *term)
{
    term->cur->fg_color_index = TERM_FG_COLOR_INDEX;
    term->cur->bg_color_index = TERM_BG_COLOR_INDEX;
    term->cur->user_fg_color = 0;
    term->cur->user_bg_color = 0;
    term->cur->ul_color = 0;
    term->cur->underline_color_set = false;
    term->cur->fg_color = term->default_fg_color;
    term->cur->bg_color = term->default_bg_color;
    term->cur->bold = false;
    term->cur->faint = false;
    term->cur->reverse = false;
    term->cur->conceal = false;
    term->cur->sgr_attr = 0;
    term->cur->origin_mode = false;
    term->cur->line_wrap = true;
    term->cur->g0_charset = 0;
    term->cur->g1_charset = 0;
    term->cur->gl_is_g1 = false;
    term->cursor_enabled = true;
    term->save_x = 0;
    term->save_y = 0;
    term->save_origin_mode = false;
    term->screen->margin_top = 0;
    term->screen->margin_bot = (uint8_t)(term->height - 1);
    term->ice_colors = false;
    term->cursor_style = 0;
    term->decsc_valid = false;
}

// Lazy clear of the given screen buffer: resets y_offset / row_idx to
// identity and marks every visible row dirty with the given erase colors.
// No cell memory is touched here -- term_clean_task drains dirty rows in
// the background (one row per tick, on both buffers), and the active
// buffer's cursor row is also cleaned synchronously when a write lands
// via term_set_cursor_position -> term_clean_line. The hard rule for
// this file is "never fill more than one row of cell memory in a single
// operation"; everything else goes through dirty-marking. Used by RIS
// for the inactive buffer, by alt-screen enter for ?1047 / ?1049, and
// by alt-screen leave to pre-stage the buffer for the next entry's clear.
static void term_mark_buf_erased(screen_buf_t *buf, uint8_t height,
                                 uint16_t fg, uint16_t bg, uint16_t ul, uint8_t attr)
{
    buf->y_offset = 0;
    for (uint8_t y = 0; y < TERM_MAX_HEIGHT; y++)
    {
        buf->row_idx[y] = y;
        buf->dirty[y] = (y < height);
    }
    for (uint8_t y = 0; y < height; y++)
    {
        buf->erase_fg_color[y] = fg;
        buf->erase_bg_color[y] = bg;
        buf->erase_ul_color[y] = ul;
        buf->erase_attr[y] = attr;
    }
    buf->all_clean = false;
}

// Common hard-reset core: everything RIS and the display-change reset share,
// stopping short of touching screen contents or cursor position. RIS then
// clears+homes (term_full_clear); the display-change path reflows the cursor
// (term_out_RIS_no_clear). Keep the two callers in sync via this helper.
static void term_reset_core(term_state_t *term)
{
    /* Drops alt mode and returns to primary. The cursor-save snapshot is
     * invalidated -- a hard reset shouldn't leave the alt save around. The
     * alt buffer's cell contents are also wiped so a later ?47 (swap-only,
     * no clear) can't surface pre-reset content. The runtime palette is also
     * restored: OSC 4 mutations don't survive a reset, matching xterm.
     * Idempotent on the second per-term call from com_out_chars. */
    memcpy(color_256_term, color_256, sizeof(color_256_term));
    term->screen = &term->bufs[0];
    term->cur = &term->screen->cs;
    term->alt_active = false;
    term->cursor_save_valid = false;
    term->ansi_state = ansi_state_C0;
    term->default_fg_color = color_256[TERM_FG_COLOR_INDEX];
    term->default_bg_color = color_256[TERM_BG_COLOR_INDEX];
    term->cursor_color = color_256[TERM_FG_COLOR_INDEX];
    term->csi_intermediate = 0;
    term_reset_sgr_and_modes(term);
    term_reset_tab_stops(term);
    term_mark_buf_erased(&term->bufs[1], term->height,
                         color_256[TERM_FG_COLOR_INDEX],
                         color_256[TERM_BG_COLOR_INDEX],
                         color_256[TERM_FG_COLOR_INDEX], 0);
}

static void term_out_RIS(term_state_t *term)
{
    term_reset_core(term);
    term_full_clear(term); /* also resets x = y = 0 on primary */
}

static void term_state_init(term_state_t *term, uint8_t width,
                            term_data_t *mem_pri, term_data_t *mem_alt)
{
    term->width = width;
    term->height = TERM_STD_HEIGHT;
    term->bufs[0].mem = mem_pri;
    term->bufs[1].mem = mem_alt;
    term->screen = &term->bufs[0];
    term->cur = &term->screen->cs;
    term->alt_active = false;
    term->cursor_save_valid = false;
    term->cursor_lit = false;
    term->cell_blink_phase = 0;
    term->cell_blink_timer = make_timeout_time_us(TERM_BLINK_TICK_US);
    term_out_RIS(term);
}

static void term_state_set_height(term_state_t *term, uint8_t height)
{
    assert(height >= 1 && height <= TERM_MAX_HEIGHT);
    while (height != term->height)
    {
        if (height > term->height)
        {
            term->height++;
            if (term->cur->y == term->height - 2)
            {
                // Reveal one row of scrollback above the view; content
                // shifts down by one so the per-screen-row arrays do too.
                term->cur->y++;
                if (!term->screen->y_offset)
                    term->screen->y_offset = TERM_MAX_HEIGHT - 1;
                else
                    term->screen->y_offset--;
                term_shift_meta_down(term, term->height - 1);
                term->screen->dirty[0] = false;
                // shift_meta_down may have copied a dirty=true flag into
                // the cursor's new row; uphold the "cur->y is never
                // dirty on the active buffer" invariant.
                term_clean_line(term, term->cur->y);
                continue;
            }
            // Expose one fresh row at the bottom: mark it dirty with the
            // default colors. The cleaner task drains it in the background;
            // the worst case is one row of stale content for a frame or two.
            uint8_t y = (uint8_t)(term->height - 1);
            term->screen->dirty[y] = true;
            term->screen->erase_fg_color[y] = term->default_fg_color;
            term->screen->erase_bg_color[y] = term->default_bg_color;
            term->screen->erase_ul_color[y] = term->default_fg_color;
            term->screen->erase_attr[y] = 0;
            term->screen->all_clean = false;
        }
        else
        {
            term->height--;
            if (term->cur->y == term->height)
            {
                term->cur->y--;
                if (++term->screen->y_offset >= TERM_MAX_HEIGHT)
                    term->screen->y_offset -= TERM_MAX_HEIGHT;
                term_shift_meta_up(term, term->height);
                // shift_meta_up may have copied a dirty=true flag into
                // the cursor's new row; uphold the "cur->y is never
                // dirty on the active buffer" invariant.
                term_clean_line(term, term->cur->y);
                continue;
            }
            // The row just hidden at logical row == term->height is no
            // longer visible; the cleaner only drains visible rows. Leave
            // it -- a later grow that re-exposes the row will mark it
            // dirty via the branch above.
        }
    }
    // DECSTBM region is meaningless across a height change. Reset both
    // buffers; the inactive one would otherwise carry stale margins that
    // could exceed the new height on a later swap.
    term->bufs[0].margin_top = 0;
    term->bufs[0].margin_bot = (uint8_t)(term->height - 1);
    term->bufs[1].margin_top = 0;
    term->bufs[1].margin_bot = (uint8_t)(term->height - 1);

    // The resize loop only tracks the active buffer's cursor. Any saved cursor
    // row (the inactive buffer's cs, and the DECSC / ?1049 / SCO snapshots) can
    // exceed the new height after a shrink; clamp them so a later swap, DECRC,
    // or RCP doesn't strand the cursor below the viewport. Same reasoning as
    // the margin reset above.
    uint8_t max_y = (uint8_t)(term->height - 1);
    screen_buf_t *inactive = &term->bufs[term->alt_active ? 0 : 1];
    if (inactive->cs.y > max_y)
        inactive->cs.y = max_y;
    if (term->cursor_save.y > max_y)
        term->cursor_save.y = max_y;
    if (term->decsc.y > max_y)
        term->decsc.y = max_y;
    if (term->save_y > max_y)
        term->save_y = max_y;
}

// Parse the param tail of SGR 38 / 48 / 58 starting at idx. Writes the
// resulting color through `color` (caller may pass a discard slot for 58)
// and returns the number of *extra* params consumed beyond the introducer
// itself. The caller then does `idx += returned; continue;` so the for-
// loop's own idx++ advances past the introducer byte.
//   ;5;N         -> 2 extras (5, N)
//   ;2;r;g;b     -> 4 extras
//   :2::r:g:b    -> 5 extras (ITU/ISO 8613-6: empty colorspace slot)
//   :2:r:g:b     -> 4 extras (no colorspace slot; libvte-style)
//   ;1           -> 1 extra (transparent flag)
// 0 means nothing matched; caller treats the introducer as a no-op and
// breaks out of its own case to advance by one.
static uint8_t sgr_color(term_state_t *term, uint8_t idx, uint16_t *color)
{
    if (idx + 2 < term->csi_param_count &&
        term->csi_param[idx + 1] == 5)
    {
        uint16_t color_idx = term->csi_param[idx + 2];
        if (color_idx < 256)
            *color = color_256_term[color_idx];
        return 2;
    }
    if (idx + 4 < term->csi_param_count &&
        term->csi_separator[idx] == ';' &&
        term->csi_param[idx + 1] == 2)
    {
        *color = SCANVIDEO_ALPHA_MASK |
                 SCANVIDEO_PIXEL_FROM_RGB8(
                     term->csi_param[idx + 2],
                     term->csi_param[idx + 3],
                     term->csi_param[idx + 4]);
        return 4;
    }
    if (idx + 4 < term->csi_param_count &&
        term->csi_separator[idx] == ':' &&
        term->csi_param[idx + 1] == 2)
    {
        // Colon (ITU-T T.416 / ISO 8613-6) truecolor. Two layouts occur:
        //   38:2::r:g:b  6 components, empty colorspace-id slot at idx+2
        //   38:2:r:g:b   5 components, no colorspace slot (libvte-style)
        // The 6-form keeps a ':' after the green channel (idx+4); the 5-form
        // ends the colon run there (next separator is ';' or the terminator).
        // That ':' is also what disambiguates when the group is followed by
        // more SGR params. Try the longer, standards-correct form first.
        if (idx + 5 < term->csi_param_count &&
            term->csi_separator[idx + 4] == ':')
        {
            *color = SCANVIDEO_ALPHA_MASK |
                     SCANVIDEO_PIXEL_FROM_RGB8(
                         term->csi_param[idx + 3],
                         term->csi_param[idx + 4],
                         term->csi_param[idx + 5]);
            return 5;
        }
        *color = SCANVIDEO_ALPHA_MASK |
                 SCANVIDEO_PIXEL_FROM_RGB8(
                     term->csi_param[idx + 2],
                     term->csi_param[idx + 3],
                     term->csi_param[idx + 4]);
        return 4;
    }
    if (idx + 1 < term->csi_param_count &&
        term->csi_param[idx + 1] == 1)
    {
        *color = *color & ~SCANVIDEO_ALPHA_MASK;
        return 1;
    }
    return 0;
}

// Recompute bg_color from bg_color_index, default bg, and the current
// blink+ice_colors state. In ice_colors mode (DECSET ?33), SGR 5/6 means
// "shift bg to its bright palette entry" instead of pulsing the cell -- the
// IBM-VGA hack the BBS scene relies on. ice_colors off (default): bg uses
// the plain palette entry and ATTR_BLINK drives the renderer's blink pulse.
static void term_update_bg_color(term_state_t *term)
{
    uint8_t idx = term->cur->bg_color_index;
    if (idx == BG_COLOR_INDEX_EXTENDED)
        term->cur->bg_color = term->cur->user_bg_color;
    else if (term->ice_colors && (term->cur->sgr_attr & ATTR_ANY_BLINK))
        term->cur->bg_color = color_256_term[idx + 8];
    else if (idx == TERM_BG_COLOR_INDEX &&
             term->default_bg_color != color_256[TERM_BG_COLOR_INDEX])
        term->cur->bg_color = term->default_bg_color;
    else
        term->cur->bg_color = color_256_term[idx];
}

// Recompute fg_color from fg_color_index, bold, faint, and the OSC-10
// default-override. Single source of truth for SGR-time fg changes:
//   - fg_color_index == FG_COLOR_INDEX_EXTENDED -> use user_fg_color (set
//     by SGR 38;5 / 38;2); bold is a color no-op for extended fg.
//   - bold && fg_color_index in 0..7 -> swap to the bright-palette slot
//     (the bold-bright trick).
//   - fg_color_index == TERM_FG_COLOR_INDEX with an OSC 10 override -> use
//     the override (override wins over bold-bright).
//   - otherwise -> color_256[fg_color_index]. SGR 90-97 stores the bright
//     slot index (8..15) directly so the lookup yields a bright color even
//     without bold; this also means SGR 22 leaves bright colors alone.
// Then if faint, halve each RGB channel via the scanvideo channel macros so
// the operation stays correct regardless of pixel-layout changes.
static void term_recompute_fg(term_state_t *term)
{
    uint16_t base;
    uint8_t idx = term->cur->fg_color_index;
    if (idx == FG_COLOR_INDEX_EXTENDED)
        base = term->cur->user_fg_color;
    else if (term->cur->bold && idx < 8)
        base = color_256_term[idx + 8];
    else if (idx == TERM_FG_COLOR_INDEX &&
             term->default_fg_color != color_256[TERM_FG_COLOR_INDEX])
        base = term->default_fg_color;
    else
        base = color_256_term[idx];
    if (term->cur->faint)
    {
        uint16_t r = SCANVIDEO_R5_FROM_PIXEL(base) >> 1;
        uint16_t g = SCANVIDEO_G5_FROM_PIXEL(base) >> 1;
        uint16_t b = SCANVIDEO_B5_FROM_PIXEL(base) >> 1;
        base = SCANVIDEO_PIXEL_FROM_RGB5(r, g, b) |
               (base & SCANVIDEO_ALPHA_MASK);
    }
    term->cur->fg_color = base;
}

static void term_out_SGR(term_state_t *term)
{
    for (uint8_t idx = 0; idx < term->csi_param_count; idx++)
    {
        uint16_t param = term->csi_param[idx];
        switch (param)
        {
        case 0: // reset
            term->cur->bold = false;
            term->cur->faint = false;
            term->cur->reverse = false;
            term->cur->conceal = false;
            term->cur->sgr_attr = 0;
            term->cur->fg_color_index = TERM_FG_COLOR_INDEX;
            term->cur->bg_color_index = TERM_BG_COLOR_INDEX;
            term->cur->ul_color = 0;
            term->cur->underline_color_set = false;
            term_recompute_fg(term);
            term_update_bg_color(term);
            break;
        case 1: // bold intensity
            term->cur->bold = true;
            term_recompute_fg(term);
            break;
        case 2: // faint -- halve channel brightness at SGR time.
                // Stored as cursor state; renderer is untouched.
            term->cur->faint = true;
            term_recompute_fg(term);
            break;
        case 3: // italic (renders only in 8x16 mode; 8x8 has no italic font)
            term->cur->sgr_attr |= ATTR_ITALIC;
            break;
        case 4: // underline
            term->cur->sgr_attr |= ATTR_UNDERLINE;
            break;
        case 5: // slow blink (~1.5 Hz)
            term->cur->sgr_attr =
                (uint8_t)((term->cur->sgr_attr & ~ATTR_ANY_BLINK) | ATTR_BLINK);
            if (term->ice_colors)
                term_update_bg_color(term);
            break;
        case 6: // rapid blink (~3 Hz)
            term->cur->sgr_attr =
                (uint8_t)((term->cur->sgr_attr & ~ATTR_ANY_BLINK) | ATTR_BLINK_FAST);
            if (term->ice_colors)
                term_update_bg_color(term);
            break;
        case 7: // reverse
            term->cur->reverse = true;
            break;
        case 8: // conceal
            term->cur->conceal = true;
            break;
        case 9: // strikethrough
            term->cur->sgr_attr |= ATTR_STRIKE;
            break;
        case 21: // double underline (per Linux console)
            term->cur->sgr_attr |= ATTR_DBL_UL;
            break;
        case 22: // normal intensity -- cancels both bold and faint
            term->cur->bold = false;
            term->cur->faint = false;
            term_recompute_fg(term);
            break;
        case 23: // italic off
            term->cur->sgr_attr &= (uint8_t)~ATTR_ITALIC;
            break;
        case 24: // underline off
            term->cur->sgr_attr &= (uint8_t)~(ATTR_UNDERLINE | ATTR_DBL_UL);
            break;
        case 25: // not blink (clears slow and rapid)
            term->cur->sgr_attr &= (uint8_t)~ATTR_ANY_BLINK;
            if (term->ice_colors)
                term_update_bg_color(term);
            break;
        case 27: // reverse off
            term->cur->reverse = false;
            break;
        case 28: // conceal off
            term->cur->conceal = false;
            break;
        case 29: // strikethrough off
            term->cur->sgr_attr &= (uint8_t)~ATTR_STRIKE;
            break;
        case 53: // overline
            term->cur->sgr_attr |= ATTR_OVERLINE;
            break;
        case 55: // overline off
            term->cur->sgr_attr &= (uint8_t)~ATTR_OVERLINE;
            break;
        case 30:
        case 31:
        case 32:
        case 33: // foreground color
        case 34:
        case 35:
        case 36:
        case 37:
            term->cur->fg_color_index = (uint8_t)(param - 30);
            term_recompute_fg(term);
            break;
        case 38:
        {
            uint16_t parsed = term->cur->fg_color;
            uint8_t consumed = sgr_color(term, idx, &parsed);
            if (consumed)
            {
                term->cur->fg_color_index = FG_COLOR_INDEX_EXTENDED;
                term->cur->user_fg_color = parsed;
                term_recompute_fg(term);
                idx += consumed;
            }
            break;
        }
        case 39:
            term->cur->fg_color_index = TERM_FG_COLOR_INDEX;
            term_recompute_fg(term);
            break;
        case 40:
        case 41:
        case 42:
        case 43: // background color
        case 44:
        case 45:
        case 46:
        case 47:
            term->cur->bg_color_index = (uint8_t)(param - 40);
            term_update_bg_color(term);
            break;
        case 48:
        {
            uint16_t parsed = term->cur->bg_color;
            uint8_t consumed = sgr_color(term, idx, &parsed);
            if (consumed)
            {
                term->cur->bg_color_index = BG_COLOR_INDEX_EXTENDED;
                term->cur->user_bg_color = parsed;
                term_update_bg_color(term);
                idx += consumed;
            }
            break;
        }
        case 49:
            term->cur->bg_color_index = TERM_BG_COLOR_INDEX;
            term_update_bg_color(term);
            break;
        case 58:
        {
            uint16_t parsed = term->cur->ul_color;
            uint8_t consumed = sgr_color(term, idx, &parsed);
            if (consumed)
            {
                term->cur->ul_color = parsed;
                term->cur->underline_color_set = true;
                idx += consumed;
            }
            break;
        }
        case 59: // default underline color
            term->cur->underline_color_set = false;
            break;
        case 90:
        case 91:
        case 92:
        case 93: // bright foreground color
        case 94:
        case 95:
        case 96:
        case 97:
            // Store the bright-palette slot directly (8..15) so SGR 22
            // leaves bright colors alone -- xterm behavior.
            term->cur->fg_color_index = (uint8_t)(param - 90 + 8);
            term_recompute_fg(term);
            break;
        case 100:
        case 101:
        case 102:
        case 103: // bright background color
        case 104:
        case 105:
        case 106:
        case 107:
            term->cur->bg_color_index = BG_COLOR_INDEX_EXTENDED;
            term->cur->user_bg_color = color_256_term[param - 100 + 8];
            term_update_bg_color(term);
            break;
        }
    }
}

// Save cursor position
static void term_out_SCP(term_state_t *term)
{
    term->save_x = term->cur->x;
    term->save_y = term->cur->y;
    term->save_origin_mode = term->cur->origin_mode;
}

// Restore cursor position
static void term_out_RCP(term_state_t *term)
{
    term->cur->origin_mode = term->save_origin_mode;
    term_set_cursor_position(term, term->save_x, term->save_y);
}

// Only the term currently being rendered should reply to host queries
static bool term_is_visible(term_state_t *term)
{
    int16_t height = vga_canvas_height();
    return (height == 180 || height == 240)
               ? term->width == 40
               : term->width == 80;
}

// Device Status Report
static void term_out_DSR(term_state_t *term)
{
    if (!term_is_visible(term))
        return;
    switch (term->csi_param[0])
    {
    case 5:
    {
        static const char ok[] = "\33[0n";
        com_in_write_reply(ok, sizeof(ok) - 1);
        break;
    }
    case 6:
    {
        unsigned x = term->cur->x;
        unsigned y = term->cur->y;
        if (x == term->width)
            x--;
        // Report region-relative under DECOM, but guard the subtraction: a
        // saved cursor restored above margin_top (DECSC/RCP straddling a
        // DECSTBM change) would otherwise underflow `unsigned` into a huge
        // bogus row.
        if (term->cur->origin_mode)
            y = (y >= term->screen->margin_top) ? (y - term->screen->margin_top) : 0;
        char buf[COM_IN_BUF_SIZE];
        int n = snprintf(buf, sizeof(buf), "\33[%u;%uR", y + 1, x + 1);
        if (n < 0 || n >= (int)sizeof(buf))
            break;
        com_in_write_reply(buf, n);
        break;
    }
    }
}

// Request Mode (DECRQM). Reply format: CSI ? Ps ; Pm $ y (private)
// or CSI Ps ; Pm $ y (ANSI). Pm: 0=unrecognized, 1=set, 2=reset,
// 3=permanently set, 4=permanently reset. No ANSI modes are
// implemented here (no IRM, LNM, etc.), so the ANSI form always
// reports 0. Private mode list mirrors DECSET/DECRST.
static void term_out_DECRQM(term_state_t *term, bool private)
{
    if (!term_is_visible(term))
        return;
    unsigned ps = term->csi_param[0];
    unsigned pm = 0;
    if (private)
    {
        switch (ps)
        {
        case 6: // DECOM
            pm = term->cur->origin_mode ? 1 : 2;
            break;
        case 7: // DECAWM
            pm = term->cur->line_wrap ? 1 : 2;
            break;
        case 12: // AT&T 610
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
        }
    }
    char buf[COM_IN_BUF_SIZE];
    int n = snprintf(buf, sizeof(buf), "\33[%s%u;%u$y",
                     private ? "?" : "", ps, pm);
    if (n < 0 || n >= (int)sizeof(buf))
        return;
    com_in_write_reply(buf, n);
}

// Primary Device Attributes. Service class 61 = VT level 1
// (VT100-compatible). We parse the DECSCUSR intermediate cleanly but
// don't implement the VT220-only set (DECUDK, NRCS, 7/8-bit C1,
// selective erase), so claiming 62 would over-advertise. Feature
// bit 22 = ANSI color.
static void term_out_DA(term_state_t *term)
{
    if (term_is_visible(term))
    {
        static const char da[] = "\33[?61;22c";
        com_in_write_reply(da, sizeof(da) - 1);
    }
}

// Secondary Device Attributes (CSI > c). Replies with a generic
// VT100-class identifier; rln uses the presence of any DA2 reply to
// distinguish modern terminals from minicom-class (broken on CSI > c
// parsing) reporters that share a bare-1 Primary DA. Terminal type
// 0 (generic VT100-class), firmware 1, ROM 0.
static void term_out_DA2(term_state_t *term)
{
    if (term_is_visible(term))
    {
        static const char da2[] = "\33[>0;1;0c";
        com_in_write_reply(da2, sizeof(da2) - 1);
    }
}

static inline bool term_tab_is_set(const term_state_t *term, uint8_t col)
{
    if (col >= TERM_MAX_WIDTH)
        return false;
    return (term->tab_stops[col >> 3] & (1u << (col & 7))) != 0;
}

// Move cursor right to the next tab stop. Pins at width-1 if no stop ahead
// (Linux console behavior: HT clamps, doesn't wrap).
static void term_out_HT(term_state_t *term)
{
    if (term->cur->x >= term->width)
        return;
    uint8_t target = (uint8_t)(term->width - 1);
    for (uint8_t col = (uint8_t)(term->cur->x + 1); col < term->width; col++)
    {
        if (term_tab_is_set(term, col))
        {
            target = col;
            break;
        }
    }
    uint8_t step = (uint8_t)(target - term->cur->x);
    term->ptr += step;
    term->cur->x = target;
}

// Move cursor left to the previous tab stop. Pins at 0 if none behind.
static void term_out_CBT(term_state_t *term)
{
    uint16_t n = term->csi_param[0];
    if (n < 1)
        n = 1;
    while (n-- > 0 && term->cur->x > 0)
    {
        uint8_t target = 0;
        for (int col = (int)term->cur->x - 1; col >= 0; col--)
        {
            if (term_tab_is_set(term, (uint8_t)col))
            {
                target = (uint8_t)col;
                break;
            }
        }
        uint8_t step = (uint8_t)(term->cur->x - target);
        term->ptr -= step;
        term->cur->x = target;
    }
}

// Forward Pn tabs.
static void term_out_CHT(term_state_t *term)
{
    uint16_t n = term->csi_param[0];
    if (n < 1)
        n = 1;
    while (n-- > 0 && term->cur->x < term->width - 1)
        term_out_HT(term);
}

// Tab Clear. Ps=0 (default) clears the stop at the cursor; Ps=3 clears all.
static void term_out_TBC(term_state_t *term)
{
    uint16_t ps = term->csi_param[0];
    if (ps == 3)
    {
        memset(term->tab_stops, 0, sizeof(term->tab_stops));
    }
    else if (ps == 0)
    {
        if (term->cur->x < TERM_MAX_WIDTH)
            term->tab_stops[term->cur->x >> 3] &= (uint8_t)~(1u << (term->cur->x & 7));
    }
}

// ?1049 entry snapshots the primary cursor + SGR; ?1049 exit restores it.
// cursor_state_t is the same type as the active cursor, so save/restore is
// a single struct copy.
static void term_save_cursor_state(term_state_t *term)
{
    term->cursor_save = *term->cur;
    term->cursor_save_valid = true;
}

static void term_restore_cursor_state(term_state_t *term)
{
    if (!term->cursor_save_valid)
        return;
    uint8_t x = term->cursor_save.x;
    uint8_t y = term->cursor_save.y;
    *term->cur = term->cursor_save;
    term_set_cursor_position(term, x, y);
}

// Mark the currently active screen lazy-erased at the current SGR. Cleaner
// task drains the rows in the background; the cursor row is cleaned
// synchronously by the caller's term_set_cursor_position. The bound on
// visible stale content is "a few scanlines for one or two frames".
static void term_mark_screen_erased(term_state_t *term)
{
    uint16_t fg, bg, ul;
    uint8_t attr;
    term_emit_erase(term, &fg, &bg, &ul, &attr);
    term_mark_buf_erased(term->screen, term->height, fg, bg, ul, attr);
}

// Swap to the buffer at idx (0 = primary, 1 = alt). Both screen and cur
// pointers move together.
static inline void term_set_screen(term_state_t *term, uint8_t idx)
{
    term->screen = &term->bufs[idx];
    term->cur = &term->screen->cs;
    term->alt_active = (idx != 0);
}

// Enter the alt screen buffer.
// - ?1049 (save_cursor + clear_on_entry): each screen keeps its own cursor;
//   alt cursor is reset to a clean state by lazy clear + home. On exit
//   the primary cursor returns automatically via the swap.
// - ?1047 (clear_on_entry only): cursor follows the copied alt cs but the
//   screen is lazy-cleared so stale alt content doesn't show through.
// - ?47 (neither flag): the cursor is logically shared across the swap,
//   and alt content is preserved.
static void term_enter_alt(term_state_t *term, bool save_cursor, bool clear_on_entry)
{
    if (term->alt_active)
        return;
    if (save_cursor)
        term_save_cursor_state(term);
    /* Seed alt's cs from primary unconditionally. ?47/?1047 need this so the
     * cursor follows across the swap; ?1049 needs it so the lazy clear uses
     * primary's SGR rather than alt's never-initialized (zero = black/black)
     * cs, which otherwise paints the cleared screen invisibly. */
    term->bufs[1].cs = term->bufs[0].cs;
    term_set_screen(term, 1);
    if (clear_on_entry)
    {
        term_mark_screen_erased(term);
        term_set_cursor_position(term, 0, 0);
    }
    else
    {
        // Route through term_set_cursor_position so the destination row is
        // cleaned (alt buffer may have lazy-dirty rows from a prior leave).
        term_set_cursor_position(term, term->cur->x, term->cur->y);
    }
}

// Leave the alt screen buffer.
// - ?1049: restore the saved cursor snapshot. mark_for_reentry=true
//   pre-stages alt with a lazy erase so the cleaner task drains it while
//   primary is active; by the next ?1049 entry most rows are already clean.
// - ?1047: cursor follows alt's cs back into primary's slot.
//   mark_for_reentry=true for the same reason as ?1049.
// - ?47: cursor follows, alt content preserved (mark_for_reentry=false).
static void term_leave_alt(term_state_t *term, bool restore_cursor, bool mark_for_reentry)
{
    if (!term->alt_active)
        return;
    if (mark_for_reentry)
        term_mark_screen_erased(term);
    if (!restore_cursor)
        term->bufs[0].cs = term->bufs[1].cs; /* cursor follows for ?47/?1047 */
    term_set_screen(term, 0);
    if (restore_cursor && term->cursor_save_valid)
        term_restore_cursor_state(term);
    else
        term_set_cursor_position(term, term->cur->x, term->cur->y);
}

// Scroll the inclusive region [top, bot] up by n rows in a single pass.
// Content moves up; the n newly-exposed rows at the bottom are marked
// dirty with current SGR erase colors. Cell writes are deferred to
// term_clean_task. Caller is responsible for cursor positioning. If the
// caller needs the cursor's row blanked synchronously (LF/RI), it should
// follow up with term_clean_line(term, cur->y).
static void term_region_scroll_up(term_state_t *term, uint8_t top,
                                  uint8_t bot, uint8_t n)
{
    if (top > bot || n == 0)
        return;
    uint8_t region_h = (uint8_t)(bot - top + 1);
    if (n > region_h)
        n = region_h;

    // Rotate row_idx within the region by n: save the top n slots, shift
    // the rest down by n, place the saved n at the bottom of the region.
    uint8_t y_off = term->screen->y_offset;
    uint8_t saved[TERM_MAX_HEIGHT];
    for (uint8_t i = 0; i < n; i++)
        saved[i] = term->screen->row_idx[term_buf_slot(y_off, (uint8_t)(top + i))];
    for (uint8_t i = top; (uint8_t)(i + n) <= bot; i++)
    {
        uint8_t dst = term_buf_slot(y_off, i);
        uint8_t src = term_buf_slot(y_off, (uint8_t)(i + n));
        term->screen->row_idx[dst] = term->screen->row_idx[src];
    }
    for (uint8_t i = 0; i < n; i++)
        term->screen->row_idx[term_buf_slot(y_off, (uint8_t)(bot - n + 1 + i))] = saved[i];

    // Content moved up by n, so each row inherits the metadata of the
    // row n positions below it.
    for (uint8_t i = top; (uint8_t)(i + n) <= bot; i++)
    {
        term->screen->dirty[i] = term->screen->dirty[i + n];
        term->screen->erase_fg_color[i] = term->screen->erase_fg_color[i + n];
        term->screen->erase_bg_color[i] = term->screen->erase_bg_color[i + n];
        term->screen->erase_ul_color[i] = term->screen->erase_ul_color[i + n];
        term->screen->erase_attr[i] = term->screen->erase_attr[i + n];
    }

    // Mark the n newly-exposed rows at the bottom dirty (lazy clear).
    term_mark_rows_erase(term, (uint8_t)(bot - n + 1), (uint8_t)(bot + 1));
}

// Symmetric to term_region_scroll_up: shifts content down by n rows; the
// n newly-exposed rows at the top are marked dirty (lazy clear).
static void term_region_scroll_down(term_state_t *term, uint8_t top,
                                    uint8_t bot, uint8_t n)
{
    if (top > bot || n == 0)
        return;
    uint8_t region_h = (uint8_t)(bot - top + 1);
    if (n > region_h)
        n = region_h;

    // Rotate row_idx within the region by n: save the bottom n slots,
    // shift the rest up by n, place the saved n at the top of the region.
    uint8_t y_off = term->screen->y_offset;
    uint8_t saved[TERM_MAX_HEIGHT];
    for (uint8_t i = 0; i < n; i++)
        saved[i] = term->screen->row_idx[term_buf_slot(y_off, (uint8_t)(bot - n + 1 + i))];
    for (int i = (int)bot; i - (int)n >= (int)top; i--)
    {
        uint8_t dst = term_buf_slot(y_off, (uint8_t)i);
        uint8_t src = term_buf_slot(y_off, (uint8_t)(i - n));
        term->screen->row_idx[dst] = term->screen->row_idx[src];
    }
    for (uint8_t i = 0; i < n; i++)
        term->screen->row_idx[term_buf_slot(y_off, (uint8_t)(top + i))] = saved[i];

    // Content moved down by n, so each row inherits the metadata of the
    // row n positions above it.
    for (int i = (int)bot; i - (int)n >= (int)top; i--)
    {
        term->screen->dirty[i] = term->screen->dirty[i - n];
        term->screen->erase_fg_color[i] = term->screen->erase_fg_color[i - n];
        term->screen->erase_bg_color[i] = term->screen->erase_bg_color[i - n];
        term->screen->erase_ul_color[i] = term->screen->erase_ul_color[i - n];
        term->screen->erase_attr[i] = term->screen->erase_attr[i - n];
    }

    // Mark the n newly-exposed rows at the top dirty (lazy clear).
    term_mark_rows_erase(term, top, (uint8_t)(top + n));
}

// Visually-empty test: a space, default background, and no rendered line
// attributes (underline/strike/overline). A lazy-dirty row is judged from its
// pending erase colors. Logical-row indexed (matches dirty[]/term_row_ptr).
static bool term_row_is_blank(const term_state_t *term, uint8_t y)
{
    if (term->screen->dirty[y])
        return term->screen->erase_bg_color[y] == term->default_bg_color &&
               term->screen->erase_attr[y] == 0;
    const term_data_t *row = term_row_ptr(term, y);
    for (uint8_t x = 0; x < term->width; x++)
        if (row[x].font_code != ' ' ||
            row[x].bg_color != term->default_bg_color ||
            row[x].attributes != 0)
            return false;
    return true;
}

// Reposition the cursor after a display-change reset without homing or
// clearing: drop onto the bottom-most row that still holds data, keeping the
// current column. If every row below the cursor is already blank, stay put.
// Landing on a row that has data is fine -- no CR, and no scroll at the bottom.
static void term_reflow_cursor_after_display_change(term_state_t *term)
{
    uint8_t y = term->cur->y;
    if (y >= term->height) // defensive; set_height already clamps cur->y
        y = (uint8_t)(term->height - 1);
    uint8_t target = y;
    for (uint8_t r = (uint8_t)(y + 1); r < term->height; r++)
        if (!term_row_is_blank(term, r))
            target = r;
    term_set_cursor_position(term, term->cur->x, target);
}

// Like RIS, but preserves the primary screen and reflows the cursor instead of
// clearing/homing. Out-of-band (PIX DISPLAY xreg, not com_out_chars), so it
// restarts the cursor blink itself.
static void term_out_RIS_no_clear(term_state_t *term)
{
    term_reset_core(term);
    term_reflow_cursor_after_display_change(term);
    term_cursor_restart_blink(term);
}

static void term_out_LF(term_state_t *term)
{
    if (term->cur->y < term->screen->margin_bot)
    {
        // Above the region's bottom: simply move down.
        ++term->cur->y;
    }
    else if (term->cur->y > term->screen->margin_bot)
    {
        // Below the region: walk down but never scroll; pin at last row.
        if (term->cur->y < term->height - 1)
            ++term->cur->y;
    }
    else
    {
        bool full_screen = (term->screen->margin_top == 0 &&
                            term->screen->margin_bot == term->height - 1);
        if (full_screen)
        {
            // Fast path: rotate the viewport via y_offset, mark the new
            // bottom row dirty. The term_clean_line below cleans it before
            // any glyph lands.
            if (++term->screen->y_offset == TERM_MAX_HEIGHT)
                term->screen->y_offset = 0;
            term_shift_meta_up(term, term->height - 1);
            term_mark_rows_erase(term, (uint8_t)(term->height - 1), term->height);
        }
        else
        {
            term_region_scroll_up(term, term->screen->margin_top, term->screen->margin_bot, 1);
        }
    }
    term_refresh_cursor_ptr(term);
}

// Reverse Index (ESC M) — dual of LF. Above margin_top: cursor moves up,
// no scroll. At margin_top: scrolls the region (or whole screen) down by
// one row. Below margin_top inside the region: just moves up.
static void term_out_RI(term_state_t *term)
{
    if (term->cur->y != term->screen->margin_top)
    {
        if (term->cur->y > 0)
            --term->cur->y;
        term_refresh_cursor_ptr(term);
        return;
    }

    bool full_screen = (term->screen->margin_top == 0 &&
                        term->screen->margin_bot == term->height - 1);
    if (full_screen)
    {
        if (term->screen->y_offset == 0)
            term->screen->y_offset = TERM_MAX_HEIGHT - 1;
        else
            --term->screen->y_offset;
        term_shift_meta_down(term, term->height - 1);
        term_mark_rows_erase(term, 0, 1);
    }
    else
    {
        term_region_scroll_down(term, term->screen->margin_top, term->screen->margin_bot, 1);
    }
    term_refresh_cursor_ptr(term);
}

static void term_out_CR(term_state_t *term)
{
    term->ptr -= term->cur->x;
    term->cur->x = 0;
}

static void term_out_glyph(term_state_t *term, char ch)
{
    if (term->cur->x == term->width)
    {
        // Pending-wrap state. Only reachable with DECAWM on: the clamp at
        // the end of this function keeps cur->x <= width-1 when wrap is off.
        term_out_CR(term);
        term_out_LF(term);
    }
    uint16_t fg, bg;
    uint8_t attr;
    term_emit_attrs(term, &fg, &bg, &attr);
    // Apply DEC Special Graphics substitution at render time, not at the
    // font_code level: store the original byte and flag the cell so the
    // renderer can pull from font_dec_*. The 0x80..0xFF font range stays
    // reserved for the active codepage.
    uint8_t active_set = term->cur->gl_is_g1 ? term->cur->g1_charset : term->cur->g0_charset;
    uint8_t byte = (uint8_t)ch;
    if (active_set == 1 && byte >= 0x5F && byte <= 0x7E)
        attr |= ATTR_DEC;
    term->ptr->font_code = byte;
    term->ptr->fg_color = fg;
    term->ptr->bg_color = bg;
    term->ptr->ul_color = term->cur->underline_color_set ? term->cur->ul_color : fg;
    term->ptr->attributes = attr;
    // Advance unless DECAWM is off and we just wrote at the last visible
    // column — other terminals clamp here instead of parking at col W+1,
    // and the pending-wrap state is only meaningful with wrap on.
    if (term->cur->line_wrap || term->cur->x < (uint16_t)(term->width - 1))
    {
        term->cur->x++;
        term->ptr++;
    }
}

// Cursor up
static void term_out_CUU(term_state_t *term)
{
    uint16_t rows = term->csi_param[0];
    if (rows < 1)
        rows = 1;
    // Soft fence: cursor already inside the region cannot cross margin_top.
    uint8_t top_fence = (term->cur->y >= term->screen->margin_top) ? term->screen->margin_top : 0;
    uint16_t y = term->cur->y;
    while (rows && y > top_fence)
        --rows, --y;
    term->cur->y = y;
    term_refresh_cursor_ptr(term);
}

// Cursor down
static void term_out_CUD(term_state_t *term)
{
    uint16_t rows = term->csi_param[0];
    if (rows < 1)
        rows = 1;
    // Soft fence: cursor already inside the region cannot cross margin_bot.
    uint8_t bot_fence = (term->cur->y <= term->screen->margin_bot)
                            ? term->screen->margin_bot
                            : (uint8_t)(term->height - 1);
    uint16_t y = term->cur->y;
    while (rows && y < bot_fence)
        --rows, ++y;
    term->cur->y = y;
    term_refresh_cursor_ptr(term);
}

// Cursor forward
static void term_out_CUF(term_state_t *term)
{
    uint16_t cols = term->csi_param[0];
    if (cols < 1)
        cols = 1;
    if (cols > term->width - term->cur->x)
        cols = term->width - term->cur->x;
    term->ptr += cols;
    term->cur->x += cols;
}

// Cursor backward
static void term_out_CUB(term_state_t *term)
{
    uint16_t cols = term->csi_param[0];
    if (cols < 1)
        cols = 1;
    if (cols > term->cur->x)
        cols = term->cur->x;
    term->ptr -= cols;
    term->cur->x -= cols;
}

// Delete characters
static void term_out_DCH(term_state_t *term)
{
    unsigned max_chars = term->width - term->cur->x;
    uint16_t chars = term->csi_param[0];
    if (chars < 1)
        chars = 1;
    if (chars > max_chars)
        chars = max_chars;

    term_data_t *row = term_row_ptr(term, term->cur->y);
    unsigned tail = (unsigned)term->width - chars;
    for (unsigned i = term->cur->x; i < tail; i++)
        row[i] = row[i + chars];

    uint16_t fg, bg, ul;
    uint8_t attr;
    term_emit_erase(term, &fg, &bg, &ul, &attr);
    fill_cells(row, tail, term->width, fg, bg, ul, attr);
}

// Insert Pn blank characters at the cursor on the current row; cells past
// the right margin are pushed off the line and lost.
static void term_out_ICH(term_state_t *term)
{
    unsigned max_chars = term->width - term->cur->x;
    uint16_t chars = term->csi_param[0];
    if (chars < 1)
        chars = 1;
    if (chars > max_chars)
        chars = max_chars;

    term_data_t *row = term_row_ptr(term, term->cur->y);
    for (int i = (int)term->width - 1; i >= (int)term->cur->x + (int)chars; i--)
        row[i] = row[i - chars];

    uint16_t fg, bg, ul;
    uint8_t attr;
    term_emit_erase(term, &fg, &bg, &ul, &attr);
    fill_cells(row, term->cur->x, term->cur->x + chars, fg, bg, ul, attr);
}

// Insert Pn lines at the cursor. Only valid inside [margin_top, margin_bot];
// cursor moves to column 1 (xterm/Linux console behavior).
static void term_out_IL(term_state_t *term)
{
    if (term->cur->y < term->screen->margin_top || term->cur->y > term->screen->margin_bot)
        return;
    uint16_t n = term->csi_param[0];
    if (n < 1)
        n = 1;
    term_region_scroll_down(term, term->cur->y, term->screen->margin_bot, (uint8_t)n);
    term_set_cursor_position(term, 0, term->cur->y);
}

// Delete Pn lines at the cursor. Only valid inside [margin_top, margin_bot];
// cursor moves to column 1.
static void term_out_DL(term_state_t *term)
{
    if (term->cur->y < term->screen->margin_top || term->cur->y > term->screen->margin_bot)
        return;
    uint16_t n = term->csi_param[0];
    if (n < 1)
        n = 1;
    term_region_scroll_up(term, term->cur->y, term->screen->margin_bot, (uint8_t)n);
    term_set_cursor_position(term, 0, term->cur->y);
}

// Erase Pn characters from the cursor. Cursor doesn't move; wrap chain
// is unchanged (this is a paint op, not a delete).
static void term_out_ECH(term_state_t *term)
{
    uint16_t n = term->csi_param[0];
    if (n < 1)
        n = 1;
    uint16_t avail = (uint16_t)(term->width - term->cur->x);
    if (n > avail)
        n = avail;
    uint16_t fg, bg, ul;
    uint8_t attr;
    term_emit_erase(term, &fg, &bg, &ul, &attr);
    fill_cells(term_row_ptr(term, term->cur->y),
               term->cur->x, term->cur->x + n, fg, bg, ul, attr);
}

// Scroll Up Pn rows within the current scroll region. Cursor unchanged.
static void term_out_SU(term_state_t *term)
{
    uint16_t n = term->csi_param[0];
    if (n < 1)
        n = 1;
    term_region_scroll_up(term, term->screen->margin_top, term->screen->margin_bot, (uint8_t)n);
    // row_idx[] rotated within the region; refresh ptr so subsequent
    // writes via term->ptr land on the new physical row for cur->y.
    term_refresh_cursor_ptr(term);
}

// Scroll Down Pn rows within the current scroll region. Cursor unchanged.
static void term_out_SD(term_state_t *term)
{
    uint16_t n = term->csi_param[0];
    if (n < 1)
        n = 1;
    term_region_scroll_down(term, term->screen->margin_top, term->screen->margin_bot, (uint8_t)n);
    term_refresh_cursor_ptr(term);
}

// Soft terminal reset (CSI ! p -- DECSTR). Resets SGR/cursor/origin/wrap/
// charset/saved-cursor and homes the cursor. Preserves: OSC 10/11/12 colors,
// tab stops, and screen contents (per VT220 spec).
static void term_out_DECSTR(term_state_t *term)
{
    term_reset_sgr_and_modes(term);
    term_set_cursor_position(term, 0, 0);
}

// Set cursor style (CSI Ps SP q -- DECSCUSR). 0 = host default, 1/2 = block,
// 3/4 = underline, 5/6 = bar.
static void term_out_DECSCUSR(term_state_t *term)
{
    uint16_t ps = term->csi_param[0];
    if (ps > 6)
        ps = 0;
    uint8_t new_style = (uint8_t)ps;
    if (term->cursor_style != new_style)
    {
        term->cursor_style = new_style;
        term_cursor_restart_blink(term);
    }
}

// Set Top and Bottom Margins (CSI Pt ; Pb r)
static void term_out_DECSTBM(term_state_t *term)
{
    uint16_t top = term->csi_param[0];
    uint16_t bot = (term->csi_param_count >= 2) ? term->csi_param[1] : 0;
    if (top < 1)
        top = 1;
    if (bot < 1)
        bot = term->height;
    if (top > term->height)
        top = term->height;
    if (bot > term->height)
        bot = term->height;
    if (top >= bot)
        return; // invalid region: ignore (VT100)

    term->screen->margin_top = (uint8_t)(top - 1);
    term->screen->margin_bot = (uint8_t)(bot - 1);
    // Cursor homes after DECSTBM; origin-relative if DECOM is set.
    if (term->cur->origin_mode)
        term_set_cursor_position(term, 0, term->screen->margin_top);
    else
        term_set_cursor_position(term, 0, 0);
}

// Translate a 1-indexed row from a CSI parameter into an absolute logical
// row, honoring DECOM. In origin mode the row is clamped inside the scroll
// region; otherwise clamped to [1, height]. Returns the absolute row in
// the same 1-indexed convention -- caller subtracts 1 when handing off
// to term_set_cursor_position.
static uint16_t term_resolve_origin_row(term_state_t *term, uint16_t row)
{
    if (term->cur->origin_mode)
    {
        uint8_t region_h = (uint8_t)(term->screen->margin_bot - term->screen->margin_top + 1);
        if (row > region_h)
            row = region_h;
        row += term->screen->margin_top;
    }
    else if (row > term->height)
        row = term->height;
    return row;
}

// Cursor Position
static void term_out_CUP(term_state_t *term)
{
    // row and col start 1-indexed
    uint16_t row = term->csi_param[0];
    if (row < 1)
        row = 1;

    uint16_t col = term->csi_param[1];
    if (col < 1 || term->csi_param_count < 2)
        col = 1;
    if (col > term->width)
        col = term->width;

    row = term_resolve_origin_row(term, row);

    term_set_cursor_position(term, --col, --row);
}

// Cursor Horizontal Absolute (CHA) / Horizontal Position Absolute (HPA).
// Move cursor to column Pn (1-indexed); row unchanged.
static void term_out_CHA(term_state_t *term)
{
    uint16_t col = term->csi_param[0];
    if (col < 1)
        col = 1;
    if (col > term->width)
        col = term->width;
    term_set_cursor_position(term, (uint16_t)(col - 1), term->cur->y);
}

// Vertical Position Absolute (VPA). Move cursor to row Pn (1-indexed);
// column unchanged. Respects DECOM (rows are region-relative when set).
static void term_out_VPA(term_state_t *term)
{
    uint16_t row = term->csi_param[0];
    if (row < 1)
        row = 1;
    row = term_resolve_origin_row(term, row);
    term_set_cursor_position(term, term->cur->x, (uint16_t)(row - 1));
}

// Cursor Next Line (CNL): cursor down Pn rows, column 1.
static void term_out_CNL(term_state_t *term)
{
    term_out_CUD(term);
    term_out_CR(term);
}

// Cursor Preceding Line (CPL): cursor up Pn rows, column 1.
static void term_out_CPL(term_state_t *term)
{
    term_out_CUU(term);
    term_out_CR(term);
}

// Erase Line
static void term_out_EL(term_state_t *term)
{
    uint16_t erase_fg, erase_bg, erase_ul;
    uint8_t erase_a;
    term_emit_erase(term, &erase_fg, &erase_bg, &erase_ul, &erase_a);
    switch (term->csi_param[0])
    {
    case 0: // to the end of the line
    case 1: // to beginning of the line
    {
        unsigned x, end;
        if (!term->csi_param[0])
        {
            x = term->cur->x;
            end = term->width;
        }
        else
        {
            x = 0;
            // cur->x can equal width in the parked deferred-wrap state;
            // clamp so end stays inside the row.
            end = (term->cur->x < term->width) ? (unsigned)term->cur->x + 1
                                               : term->width;
        }
        fill_cells(term_row_ptr(term, term->cur->y), x, end,
                   erase_fg, erase_bg, erase_ul, erase_a);
        break;
    }
    case 2: // full line
        term->screen->dirty[term->cur->y] = true;
        term->screen->erase_fg_color[term->cur->y] = erase_fg;
        term->screen->erase_bg_color[term->cur->y] = erase_bg;
        term->screen->erase_ul_color[term->cur->y] = erase_ul;
        term->screen->erase_attr[term->cur->y] = erase_a;
        term_clean_line(term, term->cur->y);
        break;
    }
}

// Erase Display. ED 0/1/2 share the same parameter encoding as EL, so the
// cases below pass through to term_out_EL without rewriting csi_param[0] --
// ED 0 -> EL 0 (cursor to EOL), ED 1 -> EL 1 (SOL to cursor). The non-
// cursor rows are lazy-erased via term_mark_rows_erase.
static void term_out_ED(term_state_t *term)
{
    switch (term->csi_param[0])
    {
    case 0: //  to end of screen
        term_mark_rows_erase(term, term->cur->y + 1, term->height);
        term_out_EL(term);
        break;
    case 1: //  to beginning of the screen
        term_mark_rows_erase(term, 0, term->cur->y);
        term_out_EL(term);
        break;
    case 2: // full screen
        term_mark_rows_erase(term, 0, term->height);
        term_clean_line(term, term->cur->y);
        break;
    case 3: // xterm: erase scrollback. We have none, so no-op.
        break;
    }
}

static void term_out_state_C0(term_state_t *term, char ch)
{
    switch (ch)
    {
    case '\0': // NUL
    case '\a': // BEL
        break;
    case '\b': // BS
        if (term->cur->x > 0)
        {
            term->cur->x--;
            term->ptr--;
        }
        break;
    case '\t': // HT
        return term_out_HT(term);
    case '\n': // LF
    case '\v': // VT -- treated as LF (VT100 / Linux console)
        return term_out_LF(term);
    case '\f': // FF
        return term_full_clear(term);
    case '\r': // CR
        return term_out_CR(term);
    case '\16': // SO (Shift Out): activate G1
        term->cur->gl_is_g1 = true;
        break;
    case '\17': // SI (Shift In): activate G0
        term->cur->gl_is_g1 = false;
        break;
    case '\33': // ESC
        term->ansi_state = ansi_state_Fe;
        break;
    default:
        // CP437 art relies on the smiley/suit glyphs at 0x01-0x06; render
        // those and every printable byte (>= 0x20). Other unrecognized C0
        // controls are ignored, per VT/ECMA-48 -- the rest of the low CP437
        // glyphs are display-only and don't survive a stream, so the scene
        // only depends on 0x01-0x06.
        if ((uint8_t)ch >= 0x20 || ((uint8_t)ch >= 0x01 && (uint8_t)ch <= 0x06))
            return term_out_glyph(term, ch);
        break; // unrecognized C0 control: ignore
    }
}

static void term_out_state_Fe(term_state_t *term, char ch)
{
    if (ch == '[')
    {
        term->ansi_state = ansi_state_CSI;
        term->csi_param_count = 0;
        term->csi_param[0] = 0;
        term->csi_intermediate = 0;
    }
    else if (ch == ']')
    {
        term->ansi_state = ansi_state_OSC;
        term->csi_param_count = 0;
        term->csi_param[0] = 0;
    }
    else if (ch == 'N')
        term->ansi_state = ansi_state_SS2;
    else if (ch == 'O')
        term->ansi_state = ansi_state_SS3;
    else if (ch == 'c')
        term_out_RIS(term);
    else if (ch == 'M')
    {
        term_out_RI(term);
        term->ansi_state = ansi_state_C0;
    }
    else if (ch == 'D') // IND - Index (move down, scroll on margin_bot)
    {
        term_out_LF(term);
        term->ansi_state = ansi_state_C0;
    }
    else if (ch == 'E') // NEL - Next Line (CR + IND)
    {
        term_out_CR(term);
        term_out_LF(term);
        term->ansi_state = ansi_state_C0;
    }
    else if (ch == 'H') // HTS - Horizontal Tab Set at current column
    {
        if (term->cur->x < TERM_MAX_WIDTH)
            term->tab_stops[term->cur->x >> 3] |= 1u << (term->cur->x & 7);
        term->ansi_state = ansi_state_C0;
    }
    else if (ch == '7') // DECSC - Save Cursor (full state)
    {
        term->decsc = *term->cur;
        term->decsc_valid = true;
        term->ansi_state = ansi_state_C0;
    }
    else if (ch == '8') // DECRC - Restore Cursor (full state)
    {
        if (term->decsc_valid)
        {
            uint8_t x = term->decsc.x;
            uint8_t y = term->decsc.y;
            *term->cur = term->decsc;
            term_set_cursor_position(term, x, y);
        }
        else
        {
            // No prior DECSC: home the cursor (xterm/Linux console behavior).
            term_set_cursor_position(term, 0, 0);
        }
        term->ansi_state = ansi_state_C0;
    }
    else if (ch == '(')
        term->ansi_state = ansi_state_Fe_paren_G0;
    else if (ch == ')')
        term->ansi_state = ansi_state_Fe_paren_G1;
    else if (ch == '#')
        term->ansi_state = ansi_state_Fe_hash;
    else
        term->ansi_state = ansi_state_C0;
}

static void term_out_state_Fe_hash(term_state_t *term, char ch)
{
    (void)ch;
    term->ansi_state = ansi_state_C0;
}

static void term_out_state_SS2(term_state_t *term, char ch)
{
    (void)ch;
    term->ansi_state = ansi_state_C0;
}

static void term_out_state_SS3(term_state_t *term, char ch)
{
    (void)ch;
    term->ansi_state = ansi_state_C0;
}

// ESC ( c -- designate G0 charset.
// 'B' = ASCII, '0' = DEC Special Graphics. Any other byte is ignored
// (per the eat-one-byte parser policy in `man 4 console_codes`).
static void term_out_state_Fe_paren_G0(term_state_t *term, char ch)
{
    if (ch == 'B')
        term->cur->g0_charset = 0;
    else if (ch == '0')
        term->cur->g0_charset = 1;
    term->ansi_state = ansi_state_C0;
}

// ESC ) c -- designate G1 charset. Same selectors as G0.
static void term_out_state_Fe_paren_G1(term_state_t *term, char ch)
{
    if (ch == 'B')
        term->cur->g1_charset = 0;
    else if (ch == '0')
        term->cur->g1_charset = 1;
    term->ansi_state = ansi_state_C0;
}

static void term_out_CSI(term_state_t *term, char ch)
{
    if (term->csi_intermediate)
    {
        // CSI intermediates: DECSCUSR (SP q), DECSTR (! p), DECRQM ($ p).
        if (term->csi_intermediate == ' ' && ch == 'q')
            term_out_DECSCUSR(term);
        else if (term->csi_intermediate == '!' && ch == 'p')
            term_out_DECSTR(term);
        else if (term->csi_intermediate == '$' && ch == 'p')
            term_out_DECRQM(term, false);
        return;
    }
    switch (ch)
    {
    case 'm':
        term_out_SGR(term);
        break;
    case 's':
        term_out_SCP(term);
        break;
    case 'u':
        term_out_RCP(term);
        break;
    case 'n':
        term_out_DSR(term);
        break;
    case 'A':
        term_out_CUU(term);
        break;
    case 'B':
    case 'e': // VPR -- alias of CUD
        term_out_CUD(term);
        break;
    case 'C':
    case 'a': // HPR -- alias of CUF
        term_out_CUF(term);
        break;
    case 'D':
        term_out_CUB(term);
        break;
    case '@':
        term_out_ICH(term);
        break;
    case 'L':
        term_out_IL(term);
        break;
    case 'M':
        term_out_DL(term);
        break;
    case 'P':
        term_out_DCH(term);
        break;
    case 'S':
        term_out_SU(term);
        break;
    case 'T':
        term_out_SD(term);
        break;
    case 'X':
        term_out_ECH(term);
        break;
    case 'H':
    case 'f': // HVP -- alias of CUP
        term_out_CUP(term);
        break;
    case 'E':
        term_out_CNL(term);
        break;
    case 'F':
        term_out_CPL(term);
        break;
    case 'G':
    case '`': // HPA -- alias of CHA
        term_out_CHA(term);
        break;
    case 'd':
        term_out_VPA(term);
        break;
    case 'J':
        term_out_ED(term);
        break;
    case 'K':
        term_out_EL(term);
        break;
    case 'c':
        term_out_DA(term);
        break;
    case 'r':
        term_out_DECSTBM(term);
        break;
    case 'I':
        term_out_CHT(term);
        break;
    case 'Z':
        term_out_CBT(term);
        break;
    case 'g':
        term_out_TBC(term);
        break;
    case 't':
        // Window manipulation (xterm). No window manager on a fixed VGA
        // frame; silently absorb so apps that probe don't see garbage on
        // the screen.
        break;
    }
}

static void term_out_CSI_question(term_state_t *term, char ch)
{
    if (term->csi_intermediate == '$' && ch == 'p')
    {
        term_out_DECRQM(term, true);
        return;
    }
    switch (ch)
    {
    case 'h': // DECSET
        for (uint8_t i = 0; i < term->csi_param_count; i++)
        {
            switch (term->csi_param[i])
            {
            case 6: // DECOM
                term->cur->origin_mode = true;
                term_set_cursor_position(term, 0, term->screen->margin_top);
                break;
            case 7: // DECAWM (autowrap)
                term->cur->line_wrap = true;
                break;
            case 12: // AT&T 610
            case 25: // DECTCEM
                if (!term->cursor_enabled)
                {
                    term->cursor_enabled = true;
                    term_cursor_restart_blink(term);
                }
                break;
            case 33: /* iCE colors -- SGR 5/6 becomes the IBM-VGA bright-bg hack */
                if (!term->ice_colors)
                {
                    term->ice_colors = true;
                    term_update_bg_color(term);
                }
                break;
            case 47: /* legacy alt screen: swap only */
                term_enter_alt(term, false, false);
                break;
            case 1047: /* alt screen, no cursor save, clear-on-entry */
                term_enter_alt(term, false, true);
                break;
            case 1049: /* alt screen with save + clear (the modern app default) */
                term_enter_alt(term, true, true);
                break;
            }
        }
        break;
    case 'l': // DECRST
        for (uint8_t i = 0; i < term->csi_param_count; i++)
        {
            switch (term->csi_param[i])
            {
            case 6: // DECOM
                term->cur->origin_mode = false;
                term_set_cursor_position(term, 0, 0);
                break;
            case 7: // DECAWM
                term->cur->line_wrap = false;
                break;
            case 12: // AT&T 610
            case 25: // DECTCEM
                term->cursor_enabled = false;
                break;
            case 33: /* iCE colors off -- SGR 5/6 returns to real blink */
                if (term->ice_colors)
                {
                    term->ice_colors = false;
                    term_update_bg_color(term);
                }
                break;
            case 47: /* ?47 preserves alt content -- don't pre-mark */
                term_leave_alt(term, false, false);
                break;
            case 1047: /* next ?1047 entry will clear; pre-mark for amortization */
                term_leave_alt(term, false, true);
                break;
            case 1049: /* next ?1049 entry will clear; pre-mark for amortization */
                term_leave_alt(term, true, true);
                break;
            }
        }
        break;
    }
}

static void term_out_state_CSI(term_state_t *term, char ch)
{
    // ESC mid-CSI aborts the current sequence and starts a new one;
    // otherwise it would be consumed as a phantom final byte and the
    // next char would be misparsed as text.
    if (ch == '\33')
    {
        term->ansi_state = ansi_state_Fe;
        return;
    }
    // Drop digits past max, but keep counting params so the terminator still dispatches.
    if (ch >= '0' && ch <= '9')
    {
        if (term->csi_param_count < TERM_CSI_PARAM_MAX_LEN)
        {
            term->csi_param[term->csi_param_count] *= 10;
            term->csi_param[term->csi_param_count] += ch - '0';
        }
        return;
    }
    if (ch == ';' || ch == ':')
    {
        if (term->csi_param_count < TERM_CSI_PARAM_MAX_LEN)
            term->csi_separator[term->csi_param_count] = ch;
        if (++term->csi_param_count < TERM_CSI_PARAM_MAX_LEN)
            term->csi_param[term->csi_param_count] = 0;
        return;
    }
    switch (ch)
    {
    case '<':
        term->ansi_state = ansi_state_CSI_less;
        return;
    case '=':
        term->ansi_state = ansi_state_CSI_equal;
        return;
    case '>':
        term->ansi_state = ansi_state_CSI_greater;
        return;
    case '?':
        term->ansi_state = ansi_state_CSI_question;
        return;
    case ' ':
    case '!':
    case '$':
        // ECMA-48 intermediate byte: stash and keep parsing until the final.
        // Only one intermediate is tracked; multi-byte intermediates aren't
        // used by anything the Linux console subset emits.
        term->csi_intermediate = (uint8_t)ch;
        return;
    }
    if (term->csi_param_count < TERM_CSI_PARAM_MAX_LEN)
        term->csi_separator[term->csi_param_count] = 0;
    if (++term->csi_param_count > TERM_CSI_PARAM_MAX_LEN)
        term->csi_param_count = TERM_CSI_PARAM_MAX_LEN;
    switch (term->ansi_state)
    {
    case ansi_state_CSI:
        term_out_CSI(term, ch);
        break;
    case ansi_state_CSI_greater:
        // CSI > <Ps> c is Secondary DA. We answer with a generic
        // identifier so rln (and any future fingerprinter) can tell
        // us apart from minicom-class peers that drop the sequence.
        // Any other CSI > final byte is silently consumed.
        if (ch == 'c')
            term_out_DA2(term);
        break;
    case ansi_state_CSI_less:
    case ansi_state_CSI_equal:
        // Private CSI parameter bytes; recognize the sequence so digits
        // don't misparse, then discard. CSI = c (tertiary DA / DECRPTUI)
        // needs a DCS-framed reply that nothing here actually wants;
        // CSI < c isn't a standard query — silence is the right answer
        // for both.
        break;
    case ansi_state_CSI_question:
        term_out_CSI_question(term, ch);
        break;
    default:
        break;
    }
    term->ansi_state = ansi_state_C0;
}

// Dispatched on OSC terminator (BEL or ST). Implements the dynamic-color
// subset of xterm OSC sequences:
//   OSC 4 ; N ; #rrggbb  set palette entry N
//   OSC 10 ; #rrggbb     set default foreground
//   OSC 11 ; #rrggbb     set default background
//   OSC 12 ; #rrggbb     set cursor color
//   OSC 104              reset all palette entries
//   OSC 104 ; N          reset palette entry N
//   OSC 110              reset default foreground
//   OSC 111              reset default background
//   OSC 112              reset cursor color
// Spec format restricted to "#rrggbb"; other OSC codes/specs are silently
// ignored for forward compatibility.
static void term_out_OSC(term_state_t *term)
{
    uint8_t count = term->csi_param_count;
    bool spec_ok = (count == 8);
    bool empty = (count == 0);
    bool osc104_indexed = (count == 10);
    uint16_t packed = 0;
    if (spec_ok)
        packed = SCANVIDEO_ALPHA_MASK | SCANVIDEO_PIXEL_FROM_RGB8(
                                            term->csi_param[1],
                                            term->csi_param[2],
                                            term->csi_param[3]);
    switch (term->csi_param[0])
    {
    case 4:
        if (spec_ok)
            color_256_term[term->csi_param[4]] = packed;
        break;
    case 104:
        if (empty)
            memcpy(color_256_term, color_256, sizeof(color_256_term));
        else if (osc104_indexed)
            color_256_term[term->csi_param[4]] = color_256[term->csi_param[4]];
        break;
    case 10:
        if (spec_ok)
        {
            term->default_fg_color = packed;
            if (term->cur->fg_color_index == TERM_FG_COLOR_INDEX)
                term_recompute_fg(term);
        }
        break;
    case 11:
        if (spec_ok)
        {
            term->default_bg_color = packed;
            if (term->cur->bg_color_index == TERM_BG_COLOR_INDEX)
                term->cur->bg_color = packed;
        }
        break;
    case 12:
        if (spec_ok)
            term->cursor_color = packed;
        break;
    case 110:
        if (empty)
        {
            term->default_fg_color = color_256[TERM_FG_COLOR_INDEX];
            if (term->cur->fg_color_index == TERM_FG_COLOR_INDEX)
                term_recompute_fg(term);
        }
        break;
    case 111:
        if (empty)
        {
            term->default_bg_color = color_256[TERM_BG_COLOR_INDEX];
            if (term->cur->bg_color_index == TERM_BG_COLOR_INDEX)
                term->cur->bg_color = color_256[TERM_BG_COLOR_INDEX];
        }
        break;
    case 112:
        if (empty)
            term->cursor_color = color_256[TERM_FG_COLOR_INDEX];
        break;
    }
}

// Streaming OSC body parser; uses csi_param[] as scratch storage.
//   csi_param[0]      = Ps (accumulated digits)
//   csi_param[1..3]   = parsed R, G, B bytes once spec begins
//   csi_param[4]      = palette index for OSC 4 / 104
//   csi_param_count   = sub-state cursor:
//       0           collecting Ps digits
//       1           saw ';', expecting '#'
//       2..7        accumulating hex digits of #rrggbb
//       8           spec complete, awaiting terminator
//       9           OSC 4 collecting palette index; expects ';' to continue to '#'
//       10          OSC 104 collecting palette index; awaiting terminator
//       0xFF        malformed; drain until terminator
static void term_out_state_OSC(term_state_t *term, char ch)
{
    if (ch == '\a') // BEL terminator
    {
        term_out_OSC(term);
        term->ansi_state = ansi_state_C0;
        return;
    }
    if (ch == '\33') // ESC, possibly start of ST (ESC \)
    {
        term->ansi_state = ansi_state_OSC_esc;
        return;
    }
    if (term->csi_param_count == 0xFF)
        return;
    switch (term->csi_param_count)
    {
    case 0: // collecting Ps digits
        if (ch >= '0' && ch <= '9')
            term->csi_param[0] = term->csi_param[0] * 10 + (ch - '0');
        else if (ch == ';')
        {
            term->csi_param[1] = 0;
            term->csi_param[2] = 0;
            term->csi_param[3] = 0;
            term->csi_param[4] = 0;
            if (term->csi_param[0] == 4)
                term->csi_param_count = 9;
            else if (term->csi_param[0] == 104)
                term->csi_param_count = 10;
            else
                term->csi_param_count = 1;
        }
        else
            term->csi_param_count = 0xFF;
        break;
    case 1: // expect '#'
        if (ch == '#')
            term->csi_param_count = 2;
        else
            term->csi_param_count = 0xFF;
        break;
    case 9: // OSC 4 palette index; ';' advances to color spec
        if (ch >= '0' && ch <= '9')
        {
            unsigned v = term->csi_param[4] * 10u + (unsigned)(ch - '0');
            if (v > 255)
                term->csi_param_count = 0xFF;
            else
                term->csi_param[4] = (uint16_t)v;
        }
        else if (ch == ';')
            term->csi_param_count = 1;
        else
            term->csi_param_count = 0xFF;
        break;
    case 10: // OSC 104 palette index; terminator dispatches single-entry reset
        if (ch >= '0' && ch <= '9')
        {
            unsigned v = term->csi_param[4] * 10u + (unsigned)(ch - '0');
            if (v > 255)
                term->csi_param_count = 0xFF;
            else
                term->csi_param[4] = (uint16_t)v;
        }
        else
            term->csi_param_count = 0xFF;
        break;
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 7:
    {
        uint8_t hex;
        if (ch >= '0' && ch <= '9')
            hex = ch - '0';
        else if (ch >= 'a' && ch <= 'f')
            hex = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F')
            hex = ch - 'A' + 10;
        else
        {
            term->csi_param_count = 0xFF;
            break;
        }
        // States 2,3 -> R; 4,5 -> G; 6,7 -> B
        uint8_t slot = 1 + (term->csi_param_count - 2) / 2;
        term->csi_param[slot] = (term->csi_param[slot] << 4) | hex;
        term->csi_param_count++;
        break;
    }
    default: // spec already complete; any extra chars before terminator are bogus
        term->csi_param_count = 0xFF;
        break;
    }
}

static void term_out_state_OSC_esc(term_state_t *term, char ch)
{
    if (ch == '\\')
        term_out_OSC(term);
    term->ansi_state = ansi_state_C0;
}

static void term_out_char(term_state_t *term, char ch)
{
    if (ch == '\30' || ch == '\32')
    {
        // CAN and SUB both abort any in-progress escape sequence; SUB
        // additionally prints a substitute glyph, CAN is silent.
        term->ansi_state = ansi_state_C0;
        if (ch == '\32')
            term_out_glyph(term, '?');
    }
    else
        switch (term->ansi_state)
        {
        case ansi_state_C0:
            term_out_state_C0(term, ch);
            break;
        case ansi_state_Fe:
            term_out_state_Fe(term, ch);
            break;
        case ansi_state_SS2:
            term_out_state_SS2(term, ch);
            break;
        case ansi_state_SS3:
            term_out_state_SS3(term, ch);
            break;
        case ansi_state_CSI:
        case ansi_state_CSI_less:
        case ansi_state_CSI_equal:
        case ansi_state_CSI_greater:
        case ansi_state_CSI_question:
            term_out_state_CSI(term, ch);
            break;
        case ansi_state_OSC:
            term_out_state_OSC(term, ch);
            break;
        case ansi_state_OSC_esc:
            term_out_state_OSC_esc(term, ch);
            break;
        case ansi_state_Fe_hash:
            term_out_state_Fe_hash(term, ch);
            break;
        case ansi_state_Fe_paren_G0:
            term_out_state_Fe_paren_G0(term, ch);
            break;
        case ansi_state_Fe_paren_G1:
            term_out_state_Fe_paren_G1(term, ch);
            break;
        }
}

static void com_out_chars(const char *buf, int length)
{
    // Snapshot the cursor position before each char; only chars whose
    // processing actually shifts (cur->x, cur->y) trip the input gap.
    // Per-term so a canvas swap leaves both blink schedules consistent.
    for (int i = 0; i < length; i++)
    {
        uint8_t px_40 = term_40.cur->x, py_40 = term_40.cur->y;
        uint8_t px_80 = term_80.cur->x, py_80 = term_80.cur->y;
        term_out_char(&term_40, buf[i]);
        term_out_char(&term_80, buf[i]);
        if (term_40.cur->x != px_40 || term_40.cur->y != py_40)
            term_cursor_restart_blink(&term_40);
        if (term_80.cur->x != px_80 || term_80.cur->y != py_80)
            term_cursor_restart_blink(&term_80);
    }
}

void term_init(void)
{
    // prepare console
    memcpy(color_256_term, color_256, sizeof(color_256_term));
    static term_data_t term40_pri_mem[40 * TERM_MAX_HEIGHT];
    static term_data_t term40_alt_mem[40 * TERM_MAX_HEIGHT];
    static term_data_t term80_pri_mem[80 * TERM_MAX_HEIGHT];
    static term_data_t term80_alt_mem[80 * TERM_MAX_HEIGHT];
    term_state_init(&term_40, 40, term40_pri_mem, term40_alt_mem);
    term_state_init(&term_80, 80, term80_pri_mem, term80_alt_mem);
    // become part of stdout
    static stdio_driver_t term_stdio = {
        .out_chars = com_out_chars,
        .crlf_enabled = true,
    };
    stdio_set_driver_enabled(&term_stdio, true);
}

// Cursor blink timing only — toggles cursor_lit on the configured cadence
// (fast in deferred-wrap state, normal otherwise) and forces unlit when
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
    if (!time_reached(term->cursor_timer))
        return;
    term->cursor_lit = !term->cursor_lit;
    if (term->cur->x == term->width)
        term->cursor_timer = make_timeout_time_us(TERM_CURSOR_BLINK_FAST_US);
    else
        term->cursor_timer = make_timeout_time_us(TERM_CURSOR_BLINK_US);
}

// SGR-5/6 cell blink phase, per-term: a free-running 2-bit counter at bits 0-1
// (ATTR_ANY_BLINK). Bit 0 (ATTR_BLINK_FAST) flips every tick -> SGR 6 rapid;
// bit 1 (ATTR_BLINK) every two ticks -> SGR 5 slow. The renderer ANDs a cell's
// single blink bit against this value, so each cell darkens only on its own
// rate's off-phase, and both relight together at phase 0. Incremented from
// Core 0 here; read by Core 1 in the renderer hot path. Per-term so the cursor
// timer and cell blink timer share an owner instead of crossing a global.
static void term_cell_blink_phase_task(term_state_t *term)
{
    if (time_reached(term->cell_blink_timer))
    {
        term->cell_blink_phase =
            (uint8_t)((term->cell_blink_phase + 1) & ATTR_ANY_BLINK);
        term->cell_blink_timer = make_timeout_time_us(TERM_BLINK_TICK_US);
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

static inline bool __attribute__((optimize("O3")))
term_render_320(int16_t scanline_id, uint16_t *rgb)
{
    scanline_id -= term_scanline_begin;
    const uint8_t scanrow = (uint8_t)(scanline_id & 7);
    const uint8_t *font_line = &font8[scanrow * 256];
    const uint8_t *font_line_dec = &font_dec_8[scanrow * 32];
    // Each line attribute lights up only on the scan rows where its stroke
    // appears in the cell. The renderer's inner branch ANDs the cell's attr
    // with line_mask; a hit forces bits = 0xFF (a solid horizontal stroke
    // at this scan row). 8x8 cell layout:
    //   row 0     = overline
    //   row 4     = strikethrough (middle)
    //   row 5,7   = double underline
    //   row 7     = underline
    // blink_mask is term_40's 2-bit cell-blink phase: bit 0 (ATTR_BLINK_FAST)
    // is the rapid off-half, bit 1 (ATTR_BLINK) the slow off-half. Each cell's
    // single blink bit ANDs against it, so the two rates pulse independently.
    const uint8_t blink_mask = term_40.cell_blink_phase;
    const uint8_t line_mask =
        (uint8_t)((scanrow == 7 ? ATTR_UNDERLINE : 0) |
                  ((scanrow == 7 || scanrow == 5) ? ATTR_DBL_UL : 0) |
                  (scanrow == 4 ? ATTR_STRIKE : 0) |
                  (scanrow == 0 ? ATTR_OVERLINE : 0));
    // SGR 58 underline color applies only on underline scanrows; hoists
    // out of the inner loop. ATTR_STRIKE and ATTR_OVERLINE always use fg.
    const bool ul_row = (line_mask & (ATTR_UNDERLINE | ATTR_DBL_UL)) != 0;
    const uint8_t logical_row = (uint8_t)(scanline_id / 8);
    term_data_t *cell = term_row_ptr(&term_40, logical_row);
    uint16_t *const rgb_line = rgb;
    for (int i = 0; i < 40; i++, cell++)
    {
        uint8_t attr = cell->attributes;
        uint8_t bits = font_line[cell->font_code];
        uint16_t fg = cell->fg_color;
        uint16_t bg = cell->bg_color;
        if (attr)
        {
            if (attr & ATTR_DEC)
                bits = font_line_dec[(uint8_t)(cell->font_code - 0x5F)];
            if (attr & blink_mask)
                fg = bg;
            if (attr & line_mask)
            {
                bits = 0xFF;
                if (ul_row)
                    fg = cell->ul_color;
            }
        }
        modes_render_1bpp(rgb, bits, bg, fg);
        rgb += 8;
    }
    // Cursor overlay: at most one cell per scanline. Patches the rendered
    // pixels in place; cursor wins over ATTR_BLINK on its cell. Steady
    // styles (2/4/6) draw regardless of cursor_lit -- blink_cursor only
    // owns the timing, the style decides whether the off half is visible.
    if (logical_row == term_40.cur->y &&
        term_40.cursor_enabled &&
        (term_40.cursor_lit ||
         term_40.cursor_style == 2 ||
         term_40.cursor_style == 4 ||
         term_40.cursor_style == 6))
    {
        uint8_t cx = term_40.cur->x;
        // Wrap-pending: cursor is parked past the rightmost cell. Always
        // render the full block here regardless of cursor_style — an
        // underline strip or 1px bar at width-1 is too easy to miss for
        // a state the fast blink already flags as "different."
        bool wrap_pending = (cx >= term_40.width);
        if (wrap_pending)
            cx = (uint8_t)(term_40.width - 1);
        uint16_t *crgb = rgb_line + (uint32_t)cx * 8;
        const uint16_t cursor_color = term_40.cursor_color;
        switch (wrap_pending ? 1u : term_40.cursor_style)
        {
        case 3:
        case 4: // underline: solid strip at scanrow 7 only
            if (scanrow == 7)
                modes_render_1bpp(crgb, 0xFF, cursor_color, cursor_color);
            break;
        case 5:
        case 6: // bar: 1px at left edge on 8x8
            crgb[0] = cursor_color;
            break;
        default:
        { // 0/1/2 -- block: invert cell with cursor color
            term_data_t *cp = term_row_ptr(&term_40, logical_row) + cx;
            uint8_t cattr = cp->attributes;
            uint8_t cbits = font_line[cp->font_code];
            if (cattr & ATTR_DEC)
                cbits = font_line_dec[(uint8_t)(cp->font_code - 0x5F)];
            if (cattr & line_mask)
                cbits = 0xFF;
            modes_render_1bpp(crgb, cbits, cursor_color, cp->bg_color);
            break;
        }
        }
    }
    return true;
}

static inline bool __attribute__((optimize("O3")))
term_render_640(int16_t scanline_id, uint16_t *rgb)
{
    scanline_id -= term_scanline_begin;
    const uint8_t scanrow = (uint8_t)(scanline_id & 15);
    const uint8_t *font_line = &font16[scanrow * 256];
    const uint8_t *font_line_dec = &font_dec_16[scanrow * 32];
    const uint8_t *italic_line = &italic16[scanrow * 128];
    // 8x16 cell layout:
    //   row 0      = overline
    //   row 8      = strikethrough (middle)
    //   row 13,15  = double underline
    //   row 15     = underline
    // 2-bit cell-blink phase (bit 0 = rapid off-half, bit 1 = slow off-half).
    const uint8_t blink_mask = term_80.cell_blink_phase;
    const uint8_t line_mask =
        (uint8_t)((scanrow == 15 ? ATTR_UNDERLINE : 0) |
                  ((scanrow == 15 || scanrow == 13) ? ATTR_DBL_UL : 0) |
                  (scanrow == 8 ? ATTR_STRIKE : 0) |
                  (scanrow == 0 ? ATTR_OVERLINE : 0));
    // SGR 58 underline color applies only on underline scanrows; hoists
    // out of the inner loop. ATTR_STRIKE and ATTR_OVERLINE always use fg.
    const bool ul_row = (line_mask & (ATTR_UNDERLINE | ATTR_DBL_UL)) != 0;
    const uint8_t logical_row = (uint8_t)(scanline_id / 16);
    term_data_t *cell = term_row_ptr(&term_80, logical_row);
    uint16_t *const rgb_line = rgb;
    for (int i = 0; i < 80; i++, cell++)
    {
        uint8_t attr = cell->attributes;
        uint8_t bits = font_line[cell->font_code];
        uint16_t fg = cell->fg_color;
        uint16_t bg = cell->bg_color;
        if (attr)
        {
            if (attr & ATTR_DEC)
                bits = font_line_dec[(uint8_t)(cell->font_code - 0x5F)];
            else if ((attr & ATTR_ITALIC) && cell->font_code < 0x80)
                bits = italic_line[cell->font_code];
            if (attr & blink_mask)
                fg = bg;
            if (attr & line_mask)
            {
                bits = 0xFF;
                if (ul_row)
                    fg = cell->ul_color;
            }
        }
        modes_render_1bpp(rgb, bits, bg, fg);
        rgb += 8;
    }
    // Cursor overlay: at most one cell per scanline. Underline strip is the
    // bottom 2 rows on 8x16; bar is 2px wide for proportionality. Steady
    // styles (2/4/6) draw regardless of cursor_lit -- blink_cursor only
    // owns the timing, the style decides whether the off half is visible.
    if (logical_row == term_80.cur->y &&
        term_80.cursor_enabled &&
        (term_80.cursor_lit ||
         term_80.cursor_style == 2 ||
         term_80.cursor_style == 4 ||
         term_80.cursor_style == 6))
    {
        uint8_t cx = term_80.cur->x;
        // Wrap-pending: cursor is parked past the rightmost cell. Always
        // render the full block here regardless of cursor_style — an
        // underline strip or 2px bar at width-1 is too easy to miss for
        // a state the fast blink already flags as "different."
        bool wrap_pending = (cx >= term_80.width);
        if (wrap_pending)
            cx = (uint8_t)(term_80.width - 1);
        uint16_t *crgb = rgb_line + (uint32_t)cx * 8;
        const uint16_t cursor_color = term_80.cursor_color;
        switch (wrap_pending ? 1u : term_80.cursor_style)
        {
        case 3:
        case 4: // underline: solid strip at scanrows 14-15
            if (scanrow == 14 || scanrow == 15)
                modes_render_1bpp(crgb, 0xFF, cursor_color, cursor_color);
            break;
        case 5:
        case 6: // bar: 2px at left edge on 8x16
            crgb[0] = cursor_color;
            crgb[1] = cursor_color;
            break;
        default:
        { // 0/1/2 -- block: invert cell with cursor color
            term_data_t *cp = term_row_ptr(&term_80, logical_row) + cx;
            uint8_t cattr = cp->attributes;
            uint8_t cbits = font_line[cp->font_code];
            if (cattr & ATTR_DEC)
                cbits = font_line_dec[(uint8_t)(cp->font_code - 0x5F)];
            else if ((cattr & ATTR_ITALIC) && cp->font_code < 0x80)
                cbits = italic_line[cp->font_code];
            if (cattr & line_mask)
                cbits = 0xFF;
            modes_render_1bpp(crgb, cbits, cursor_color, cp->bg_color);
            break;
        }
        }
    }
    return true;
}

static bool __attribute__((optimize("O3")))
term_render(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    (void)(config_ptr);
    if (width == 320)
        return term_render_320(scanline_id, rgb);
    else
        return term_render_640(scanline_id, rgb);
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

bool term_prog(uint16_t *xregs)
{
    int16_t plane = xregs[2];
    int16_t scanline_begin = xregs[3];
    int16_t scanline_end = xregs[4];
    int16_t height = vga_canvas_height();
    if (!scanline_begin && !scanline_end)
    {
        // Special case to make defaults work with widescreen
        if (height == 180)
            scanline_begin = 2, scanline_end = 178;
        if (height == 360)
            scanline_begin = 4, scanline_end = 356;
    }
    if (!scanline_end)
        scanline_end = height;
    int16_t scanline_count = scanline_end - scanline_begin;
    bool use_40 = height == 180 || height == 240;

    // Check for terminal height is multiple of font height
    if (!scanline_count || scanline_count % (use_40 ? 8 : 16))
        return false;

    // Program the new scanlines
    if (vga_prog_exclusive(plane, scanline_begin, scanline_end, 0, term_render))
    {
        if (use_40)
            term_state_set_height(&term_40, scanline_count / 8);
        else
            term_state_set_height(&term_80, scanline_count / 16);
        term_scanline_begin = scanline_begin;
        return true;
    }
    return false;
}
