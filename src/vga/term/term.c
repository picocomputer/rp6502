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

// Implements the Linux console subset of ECMA-48 / VT102 with
// xterm-color (256-color, truecolor, OSC 10/11/12) extensions.
// See `man 4 console_codes`. DEC Special Graphics charset
// (ESC ( 0 / SO / SI) is supported via an attribute bit plus a
// parallel font buffer sourced from CP437. Alt screen buffer
// (?47/?1047/?1049) is supported with eager-clear-on-entry.
// 8-bit codepage encoding only -- no UTF-8 decode.
//
// Designed to keep up with 115200 bps without flow control.
// The logic herein will make more sense if you remember this:

// 1. The screen data doesn't move when scrolling. Instead, the
//    video begins rendering at y_offset and wraps around.
// 2. The screen doesn't fully clear immediately. To keep the UART
//    buffer from overflowing, lines are cleared in a background task
//    and checked as the cursor moves into them.
// 3. When lines wrap, they are marked so that you can backspace and
//    move forward and back as if it's one long virtual line. This
//    greatly simplifies line editor logic.
// 4. Each cell's `attributes` byte holds render-time flags
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
#define ATTR_UNDERLINE (1u << 0) // SGR 4
#define ATTR_STRIKE (1u << 1)    // SGR 9
#define ATTR_OVERLINE (1u << 2)  // SGR 53
#define ATTR_DBL_UL (1u << 3)    // SGR 21
#define ATTR_BLINK (1u << 4)     // SGR 5 / 6 (proper, phase-driven)
#define ATTR_DEC (1u << 5)       // cell renders from DEC graphics buffer
#define ATTR_ITALIC (1u << 6)    // SGR 3 (ASCII-only, 8x16 mode only)
#define ATTR_RENDER_MASK (ATTR_UNDERLINE | ATTR_STRIKE | ATTR_OVERLINE | \
                          ATTR_DBL_UL | ATTR_BLINK | ATTR_DEC | ATTR_ITALIC)

// SGR-state-only flags (not stored per-cell): emit-time fg/bg transforms.
// Held in cursor_state_t alongside bold/faint, not in sgr_attr.

// Global ATTR_BLINK pulse half-period (one half-cycle, on then off).
// Slightly off 333ms to avoid frame-tear sync with display refresh.
#define TERM_BLINK_PHASE_US 332700

// Cursor blink half-period: normal cell vs "stuck at right edge"
// (off-screen deferred-wrap state blinks at 2x rate so it's still noticeable).
// 0.3ms drift to avoid blinking cursor tearing.
#define TERM_CURSOR_BLINK_US 499700
#define TERM_CURSOR_BLINK_FAST_US 249700

// After each batch of printable input, the cursor is forced unlit and the
// next blink tick is armed this far out. Produces a brief dark gap that
// re-lights to the appropriate steady/blink state when it fires.
#define TERM_CURSOR_INPUT_GAP_US 2500

// Idle wakeup interval for a steady-style cursor: nothing to do until input
// arrives, but we still want a non-pathological re-check cadence.
#define TERM_CURSOR_STEADY_IDLE_MS 60000

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
} term_data_t;

// Cursor + SGR + DECSC-saved mode state. Per-screen (lives inside
// screen_buf_t), and also reused as the storage type for DECSC and ?1049
// snapshots so the field list is defined once.
// fg_color_index sentinel: fg was set via SGR 38 (256-color or RGB) and
// the recompute path should pull from user_fg_color instead of color_256[].
#define FG_COLOR_INDEX_EXTENDED 0xFF

typedef struct
{
    uint8_t x, y;
    uint8_t sgr_attr;
    uint8_t fg_color_index, bg_color_index;
    uint16_t fg_color, bg_color;
    uint16_t user_fg_color; // base color from SGR 38, used when fg_color_index == FG_COLOR_INDEX_EXTENDED
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
    bool wrapped[TERM_MAX_HEIGHT];
    bool dirty[TERM_MAX_HEIGHT];
    uint16_t erase_fg_color[TERM_MAX_HEIGHT];
    uint16_t erase_bg_color[TERM_MAX_HEIGHT];
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
    /* SCO-style save (CSI s / u) -- separate from DECSC and ?1049. */
    uint8_t save_x;
    uint8_t save_y;
    bool save_origin_mode;
    bool cursor_enabled;
    volatile bool cursor_lit; // Core 0 writes, Core 1 renderer reads. Single-byte → race-free.
    uint16_t default_fg_color;
    uint16_t default_bg_color;
    uint16_t cursor_bg_color; // configured cursor block color (OSC 12)
    term_data_t *ptr;
    absolute_time_t timer;
    ansi_state_t ansi_state;
    uint16_t csi_param[TERM_CSI_PARAM_MAX_LEN];
    char csi_separator[TERM_CSI_PARAM_MAX_LEN];
    uint8_t csi_param_count;
    uint8_t csi_intermediate;                 // ' ' or '!' captured during CSI parse
    bool ice_colors;                          // ?33 -- SGR 5/6 means bright bg (the IBM VGA hack), not blink
    uint8_t cursor_style;                     // DECSCUSR Ps (store-and-ignore)
    uint8_t tab_stops[TERM_TAB_BITMAP_BYTES]; // 1 bit per column
    cursor_state_t decsc;                     // DECSC snapshot (ESC 7 / ESC 8)
    bool decsc_valid;
} term_state_t;

static term_state_t term_40;
static term_state_t term_80;
static int16_t term_scanline_begin;

// Global blink phase shared by both terminals. Value is masked directly
// against a cell's `attributes` byte by the renderer: 0 means the blink
// half-cycle is visible (no-op), ATTR_BLINK means the off half-cycle (the
// cell's foreground is suppressed -- fg = bg). Toggled from Core 0 in
// term_task; read by Core 1 in the renderer. A torn read is impossible
// (single byte) and a one-scanline visual tear at the flip is invisible.
static volatile uint8_t term_blink_phase;
static absolute_time_t term_blink_phase_timer;

// Translate a logical row (0..height-1) into the start of its physical
// cell row. Reads y_offset and row_idx[] without locking; the renderer
// on Core 1 uses the same path. Worst case is one frame of visual tear
// while a region scroll is mid-update — same severity as today's
// y_offset++ race. No memory barrier required.
static inline term_data_t *term_row_ptr(const term_state_t *term, uint8_t y)
{
    uint8_t slot = term->screen->y_offset + y;
    if (slot >= TERM_MAX_HEIGHT)
        slot -= TERM_MAX_HEIGHT;
    return term->screen->mem + (uint32_t)term->screen->row_idx[slot] * term->width;
}

// Compute the effective fg/bg/attr a cell write should land with, given
// the current SGR state. Applies emit-time REVERSE/CONCEAL toggles; render
// bits (UL/STRIKE/OVERLINE/DBL_UL/BLINK) flow through unchanged. ATTR_DEC
// is the caller's responsibility -- only set for DEC-charset glyph cells.
// In ice_colors mode (DECSET ?33) the ATTR_BLINK bit is suppressed at the
// cell level -- the bright bg is already baked into bg_color and we don't
// want the renderer to also pulse the cell.
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
        cell_mask &= (uint8_t)~ATTR_BLINK;
    *attr = (uint8_t)(term->cur->sgr_attr & cell_mask);
}

// Make sure you call this any time you change rows.
// It will process any pending screen clears on the row.
static void term_clean_line(term_state_t *term, uint8_t y)
{
    if (!term->screen->dirty[y])
        return;
    term->screen->dirty[y] = false;
    term_data_t *row = term_row_ptr(term, y);
    uint16_t erase_fg_color = term->screen->erase_fg_color[y];
    uint16_t erase_bg_color = term->screen->erase_bg_color[y];
    uint8_t erase_attr = term->screen->erase_attr[y];
    for (size_t i = 0; i < term->width; i++)
    {
        row[i].font_code = ' ';
        row[i].fg_color = erase_fg_color;
        row[i].bg_color = erase_bg_color;
        row[i].attributes = erase_attr;
    }
}

// Set a new cursor position, 0-indexed
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
            uint8_t slot = (uint8_t)(buf->y_offset + i);
            if (slot >= TERM_MAX_HEIGHT)
                slot -= TERM_MAX_HEIGHT;
            term_data_t *row = buf->mem + (uint32_t)buf->row_idx[slot] * width;
            uint16_t fg = buf->erase_fg_color[i];
            uint16_t bg = buf->erase_bg_color[i];
            uint8_t attr = buf->erase_attr[i];
            for (size_t x = 0; x < width; x++)
            {
                row[x].font_code = ' ';
                row[x].fg_color = fg;
                row[x].bg_color = bg;
                row[x].attributes = attr;
            }
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
        term->screen->wrapped[i] = term->screen->wrapped[i + 1];
        term->screen->dirty[i] = term->screen->dirty[i + 1];
        term->screen->erase_fg_color[i] = term->screen->erase_fg_color[i + 1];
        term->screen->erase_bg_color[i] = term->screen->erase_bg_color[i + 1];
        term->screen->erase_attr[i] = term->screen->erase_attr[i + 1];
    }
}

static void term_shift_meta_down(term_state_t *term, uint8_t start)
{
    for (int i = start; i > 0; i--)
    {
        term->screen->wrapped[i] = term->screen->wrapped[i - 1];
        term->screen->dirty[i] = term->screen->dirty[i - 1];
        term->screen->erase_fg_color[i] = term->screen->erase_fg_color[i - 1];
        term->screen->erase_bg_color[i] = term->screen->erase_bg_color[i - 1];
        term->screen->erase_attr[i] = term->screen->erase_attr[i - 1];
    }
}

static void term_mark_rows_erase(term_state_t *term, uint8_t from, uint8_t to)
{
    uint16_t fg, bg;
    uint8_t attr;
    term_emit_attrs(term, &fg, &bg, &attr);
    for (uint8_t i = from; i < to; i++)
    {
        term->screen->wrapped[i] = false;
        term->screen->dirty[i] = true;
        term->screen->erase_fg_color[i] = fg;
        term->screen->erase_bg_color[i] = bg;
        term->screen->erase_attr[i] = attr;
    }
    term->screen->all_clean = false;
}

static void term_out_FF(term_state_t *term)
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
    for (size_t i = 0; i < TERM_TAB_BITMAP_BYTES; i++)
        term->tab_stops[i] = 0;
    for (uint8_t col = 0; col < term->width; col += 8)
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
    term->cur->fg_color = term->default_fg_color;
    term->cur->bg_color = term->default_bg_color;
    term->cur->bold = false;
    term->cur->faint = false;
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

// Eager clear of the given screen buffer at the supplied erase colors.
// Used on ?1049 entry so the renderer never shows stale alt content, on
// ?1047 exit to wipe the alt buffer before swap-back, and on RIS to wipe
// both buffers regardless of which is currently active. Resets y_offset
// and row_idx so the visible logical rows map straight to the freshly
// blanked physical rows.
static void term_clear_buf(screen_buf_t *buf, uint8_t width, uint8_t height,
                           uint16_t fg, uint16_t bg, uint8_t attr)
{
    buf->y_offset = 0;
    for (uint8_t y = 0; y < TERM_MAX_HEIGHT; y++)
        buf->row_idx[y] = y;
    for (uint8_t y = 0; y < height; y++)
    {
        term_data_t *row = buf->mem + (uint32_t)y * width;
        for (uint8_t x = 0; x < width; x++)
        {
            row[x].font_code = ' ';
            row[x].fg_color = fg;
            row[x].bg_color = bg;
            row[x].attributes = attr;
        }
        buf->wrapped[y] = false;
        buf->dirty[y] = false;
        buf->erase_fg_color[y] = fg;
        buf->erase_bg_color[y] = bg;
        buf->erase_attr[y] = attr;
    }
    for (uint8_t y = height; y < TERM_MAX_HEIGHT; y++)
    {
        buf->wrapped[y] = false;
        buf->dirty[y] = false;
    }
    buf->all_clean = true;
}

// Lazy variant of term_clear_buf: marks every visible row dirty with the
// given erase colors without touching cell memory. term_clean_task drains
// both buffers per tick, so the cells get filled in the background regardless
// of which buffer is active. Used on RIS so the alt clear doesn't block the
// parser for ~200 us per ESC c.
static void term_mark_buf_erased(screen_buf_t *buf, uint8_t height,
                                 uint16_t fg, uint16_t bg, uint8_t attr)
{
    buf->y_offset = 0;
    for (uint8_t y = 0; y < TERM_MAX_HEIGHT; y++)
    {
        buf->row_idx[y] = y;
        buf->wrapped[y] = false;
        buf->dirty[y] = (y < height);
    }
    for (uint8_t y = 0; y < height; y++)
    {
        buf->erase_fg_color[y] = fg;
        buf->erase_bg_color[y] = bg;
        buf->erase_attr[y] = attr;
    }
    buf->all_clean = false;
}

static void term_out_RIS(term_state_t *term)
{
    /* RIS drops alt mode and returns to primary. The cursor-save snapshot
     * is invalidated -- a hard reset shouldn't leave the alt save around.
     * The alt buffer's cell contents are also wiped so a later ?47 (swap-
     * only, no clear) can't surface pre-RIS content. */
    term->screen = &term->bufs[0];
    term->cur = &term->screen->cs;
    term->alt_active = false;
    term->cursor_save_valid = false;
    term->ansi_state = ansi_state_C0;
    term->default_fg_color = color_256[TERM_FG_COLOR_INDEX];
    term->default_bg_color = color_256[TERM_BG_COLOR_INDEX];
    term->cursor_bg_color = color_256[TERM_FG_COLOR_INDEX];
    term->csi_intermediate = 0;
    term_reset_sgr_and_modes(term);
    term_reset_tab_stops(term);
    term_mark_buf_erased(&term->bufs[1], term->height,
                         color_256[TERM_FG_COLOR_INDEX],
                         color_256[TERM_BG_COLOR_INDEX], 0);
    term_out_FF(term); /* also resets x = y = 0 on primary */
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
    term_out_RIS(term);
}

static void term_state_set_height(term_state_t *term, uint8_t height)
{
    assert(height >= 1 && height <= TERM_MAX_HEIGHT);
    while (height != term->height)
    {
        uint8_t logical_row;
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
                term->screen->wrapped[0] = false;
                term->screen->dirty[0] = false;
                continue;
            }
            // Expose one fresh row at the bottom; reset its metadata so
            // any stale dirty/wrap from a previous shrink can't resurface.
            term->screen->wrapped[term->height - 1] = false;
            term->screen->dirty[term->height - 1] = false;
            logical_row = term->height - 1;
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
                continue;
            }
            logical_row = term->height;
        }
        term_data_t *data = term_row_ptr(term, logical_row);
        for (size_t i = 0; i < term->width; i++)
        {
            data[i].font_code = ' ';
            data[i].fg_color = color_256[TERM_FG_COLOR_INDEX];
            data[i].bg_color = color_256[TERM_BG_COLOR_INDEX];
        }
    }
    // DECSTBM region is meaningless across a height change.
    term->screen->margin_top = 0;
    term->screen->margin_bot = term->height - 1;
}

// Parse the param tail of SGR 38 / 48 / 58 starting at idx. Writes the
// resulting color through `color` (caller may pass a discard slot for 58)
// and returns the number of *extra* params consumed beyond the introducer
// itself. The caller then does `idx += returned; continue;` so the for-
// loop's own idx++ advances past the introducer byte.
//   ;5;N         -> 2 extras (5, N)
//   ;2;r;g;b     -> 4 extras
//   :2::r:g:b    -> 5 extras (ITU/ISO 8613-6: empty colorspace slot)
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
            *color = color_256[color_idx];
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
    if (idx + 5 < term->csi_param_count &&
        term->csi_separator[idx] == ':' &&
        term->csi_param[idx + 1] == 2)
    {
        *color = SCANVIDEO_ALPHA_MASK |
                 SCANVIDEO_PIXEL_FROM_RGB8(
                     term->csi_param[idx + 3],
                     term->csi_param[idx + 4],
                     term->csi_param[idx + 5]);
        return 5;
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
    if (term->ice_colors && (term->cur->sgr_attr & ATTR_BLINK))
        term->cur->bg_color = color_256[idx + 8];
    else if (idx == TERM_BG_COLOR_INDEX &&
             term->default_bg_color != color_256[TERM_BG_COLOR_INDEX])
        term->cur->bg_color = term->default_bg_color;
    else
        term->cur->bg_color = color_256[idx];
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
        base = color_256[idx + 8];
    else if (idx == TERM_FG_COLOR_INDEX &&
             term->default_fg_color != color_256[TERM_FG_COLOR_INDEX])
        base = term->default_fg_color;
    else
        base = color_256[idx];
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
        case 5: // slow blink
        case 6: // rapid blink (aliased; we run one phase rate)
            term->cur->sgr_attr |= ATTR_BLINK;
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
        case 25: // not blink
            term->cur->sgr_attr &= (uint8_t)~ATTR_BLINK;
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
            uint8_t consumed = sgr_color(term, idx, &term->cur->bg_color);
            if (consumed)
                idx += consumed;
            break;
        }
        case 49:
            term->cur->bg_color_index = TERM_BG_COLOR_INDEX;
            term_update_bg_color(term);
            break;
        case 58:
        {
            // Underline color not rendered, but the params must be
            // skipped so the rest of the SGR sequence still applies.
            uint16_t discard = 0;
            uint8_t consumed = sgr_color(term, idx, &discard);
            if (consumed)
                idx += consumed;
            break;
        }
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
            term->cur->bg_color_index = (uint8_t)(param - 100);
            term->cur->bg_color = color_256[term->cur->bg_color_index + 8];
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
        com_in_write_ansi_DSR_ok();
        break;
    case 6:
    {
        unsigned x = term->cur->x;
        unsigned y = term->cur->y;
        if (x == term->width)
            x--;
        if (term->cur->origin_mode)
            y -= term->screen->margin_top;
        com_in_write_ansi_CPR(y + 1, x + 1);
        break;
    }
    }
}

// Primary Device Attributes
static void term_out_DA(term_state_t *term)
{
    if (term_is_visible(term))
        com_in_write_ansi_DA();
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
        for (int col = (int)term->cur->x - 1; col > 0; col--)
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
        for (size_t i = 0; i < TERM_TAB_BITMAP_BYTES; i++)
            term->tab_stops[i] = 0;
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

// Clear the currently active screen at the current SGR. Used on ?1049
// entry and ?1047 exit. Delegates to term_clear_buf (defined earlier so
// RIS can also call it directly without a forward declaration).
static void term_clear_screen(term_state_t *term)
{
    uint16_t fg, bg;
    uint8_t attr;
    term_emit_attrs(term, &fg, &bg, &attr);
    term_clear_buf(term->screen, term->width, term->height, fg, bg, attr);
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
//   alt cursor is reset to a clean state by clear_screen + home. On exit
//   the primary cursor returns automatically via the swap.
// - ?47 / ?1047 (neither flag): the cursor is logically shared across the
//   swap, so the current cursor state is copied into the alt screen's slot
//   before switching so writes continue from the same position.
static void term_enter_alt(term_state_t *term, bool save_cursor, bool clear_on_entry)
{
    if (term->alt_active)
        return;
    if (save_cursor)
        term_save_cursor_state(term);
    /* Seed alt's cs from primary unconditionally. ?47/?1047 need this so the
     * cursor follows across the swap; ?1049 needs it so the clear-on-entry
     * uses primary's SGR rather than alt's never-initialized (zero =
     * black/black) cs, which otherwise paints the cleared screen invisibly. */
    term->bufs[1].cs = term->bufs[0].cs;
    term_set_screen(term, 1);
    if (clear_on_entry)
    {
        term_clear_screen(term);
        term_set_cursor_position(term, 0, 0);
    }
    else
    {
        // Route through term_set_cursor_position so the destination row is
        // cleaned (alt buffer may have lazy-dirty rows after RIS).
        term_set_cursor_position(term, term->cur->x, term->cur->y);
    }
}

// Leave the alt screen buffer. ?1049 restores the saved cursor snapshot;
// ?47/?1047 copy the alt cursor back into the primary slot so the cursor
// continues from where alt left it.
static void term_leave_alt(term_state_t *term, bool restore_cursor, bool clear_on_exit)
{
    if (!term->alt_active)
        return;
    if (clear_on_exit)
        term_clear_screen(term);
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
    uint8_t saved[TERM_MAX_HEIGHT];
    for (uint8_t i = 0; i < n; i++)
    {
        uint8_t slot = (uint8_t)(term->screen->y_offset + top + i);
        if (slot >= TERM_MAX_HEIGHT)
            slot -= TERM_MAX_HEIGHT;
        saved[i] = term->screen->row_idx[slot];
    }
    for (uint8_t i = top; (uint8_t)(i + n) <= bot; i++)
    {
        uint8_t dst = (uint8_t)(term->screen->y_offset + i);
        if (dst >= TERM_MAX_HEIGHT)
            dst -= TERM_MAX_HEIGHT;
        uint8_t src = (uint8_t)(term->screen->y_offset + i + n);
        if (src >= TERM_MAX_HEIGHT)
            src -= TERM_MAX_HEIGHT;
        term->screen->row_idx[dst] = term->screen->row_idx[src];
    }
    for (uint8_t i = 0; i < n; i++)
    {
        uint8_t slot = (uint8_t)(term->screen->y_offset + (bot - n + 1) + i);
        if (slot >= TERM_MAX_HEIGHT)
            slot -= TERM_MAX_HEIGHT;
        term->screen->row_idx[slot] = saved[i];
    }

    // Shift the five logical-row metadata arrays down by n.
    for (uint8_t i = top; (uint8_t)(i + n) <= bot; i++)
    {
        term->screen->wrapped[i] = term->screen->wrapped[i + n];
        term->screen->dirty[i] = term->screen->dirty[i + n];
        term->screen->erase_fg_color[i] = term->screen->erase_fg_color[i + n];
        term->screen->erase_bg_color[i] = term->screen->erase_bg_color[i + n];
        term->screen->erase_attr[i] = term->screen->erase_attr[i + n];
    }

    // Mark the n newly-exposed rows at the bottom dirty (lazy clear).
    uint16_t fg, bg;
    uint8_t attr;
    term_emit_attrs(term, &fg, &bg, &attr);
    for (uint8_t i = (uint8_t)(bot - n + 1); i <= bot; i++)
    {
        term->screen->wrapped[i] = false;
        term->screen->dirty[i] = true;
        term->screen->erase_fg_color[i] = fg;
        term->screen->erase_bg_color[i] = bg;
        term->screen->erase_attr[i] = attr;
    }
    term->screen->all_clean = false;
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
    uint8_t saved[TERM_MAX_HEIGHT];
    for (uint8_t i = 0; i < n; i++)
    {
        uint8_t slot = (uint8_t)(term->screen->y_offset + (bot - n + 1) + i);
        if (slot >= TERM_MAX_HEIGHT)
            slot -= TERM_MAX_HEIGHT;
        saved[i] = term->screen->row_idx[slot];
    }
    for (int i = (int)bot; i - (int)n >= (int)top; i--)
    {
        uint8_t dst = (uint8_t)(term->screen->y_offset + i);
        if (dst >= TERM_MAX_HEIGHT)
            dst -= TERM_MAX_HEIGHT;
        uint8_t src = (uint8_t)(term->screen->y_offset + i - n);
        if (src >= TERM_MAX_HEIGHT)
            src -= TERM_MAX_HEIGHT;
        term->screen->row_idx[dst] = term->screen->row_idx[src];
    }
    for (uint8_t i = 0; i < n; i++)
    {
        uint8_t slot = (uint8_t)(term->screen->y_offset + top + i);
        if (slot >= TERM_MAX_HEIGHT)
            slot -= TERM_MAX_HEIGHT;
        term->screen->row_idx[slot] = saved[i];
    }

    // Shift the five logical-row metadata arrays up by n.
    for (int i = (int)bot; i - (int)n >= (int)top; i--)
    {
        term->screen->wrapped[i] = term->screen->wrapped[i - n];
        term->screen->dirty[i] = term->screen->dirty[i - n];
        term->screen->erase_fg_color[i] = term->screen->erase_fg_color[i - n];
        term->screen->erase_bg_color[i] = term->screen->erase_bg_color[i - n];
        term->screen->erase_attr[i] = term->screen->erase_attr[i - n];
    }

    // Mark the n newly-exposed rows at the top dirty (lazy clear).
    uint16_t fg, bg;
    uint8_t attr;
    term_emit_attrs(term, &fg, &bg, &attr);
    for (uint8_t i = top; i < (uint8_t)(top + n); i++)
    {
        term->screen->wrapped[i] = false;
        term->screen->dirty[i] = true;
        term->screen->erase_fg_color[i] = fg;
        term->screen->erase_bg_color[i] = bg;
        term->screen->erase_attr[i] = attr;
    }
    term->screen->all_clean = false;
}

static void term_out_LF(term_state_t *term, bool wrapping)
{
    if (wrapping)
        term->screen->wrapped[term->cur->y] = true;
    else if (term->cur->y < term->height - 1 && term->screen->wrapped[term->cur->y])
    {
        ++term->cur->y;
        return term_out_LF(term, false);
    }

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
            uint16_t fg, bg;
            uint8_t attr;
            term_emit_attrs(term, &fg, &bg, &attr);
            uint8_t y = (uint8_t)(term->height - 1);
            term->screen->wrapped[y] = false;
            term->screen->dirty[y] = true;
            term->screen->erase_fg_color[y] = fg;
            term->screen->erase_bg_color[y] = bg;
            term->screen->erase_attr[y] = attr;
            term->screen->all_clean = false;
        }
        else
        {
            term_region_scroll_up(term, term->screen->margin_top, term->screen->margin_bot, 1);
        }
    }
    term->ptr = term_row_ptr(term, term->cur->y) + term->cur->x;
    term_clean_line(term, term->cur->y);
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
        term->ptr = term_row_ptr(term, term->cur->y) + term->cur->x;
        term_clean_line(term, term->cur->y);
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
        uint16_t fg, bg;
        uint8_t attr;
        term_emit_attrs(term, &fg, &bg, &attr);
        term->screen->wrapped[0] = false;
        term->screen->dirty[0] = true;
        term->screen->erase_fg_color[0] = fg;
        term->screen->erase_bg_color[0] = bg;
        term->screen->erase_attr[0] = attr;
        term->screen->all_clean = false;
    }
    else
    {
        term_region_scroll_down(term, term->screen->margin_top, term->screen->margin_bot, 1);
    }
    term->ptr = term_row_ptr(term, term->cur->y) + term->cur->x;
    term_clean_line(term, term->cur->y);
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
        if (term->cur->line_wrap)
        {
            term_out_CR(term);
            term_out_LF(term, true);
        }
        else
        {
            --term->ptr;
            --term->cur->x;
        }
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
    if (active_set == 1 && byte >= 0x60 && byte <= 0x7E)
        attr |= ATTR_DEC;
    term->cur->x++;
    term->ptr->font_code = byte;
    term->ptr->fg_color = fg;
    term->ptr->bg_color = bg;
    term->ptr->attributes = attr;
    term->ptr++;
}

// Cursor up
static void term_out_CUU(term_state_t *term)
{
    uint16_t rows = term->csi_param[0];
    if (rows < 1)
        rows = 1;
    // Soft fence: cursor already inside the region cannot cross margin_top.
    uint8_t floor = (term->cur->y >= term->screen->margin_top) ? term->screen->margin_top : 0;
    uint16_t y = term->cur->y;
    while (rows && y > floor)
        --rows, --y;
    term->cur->y = y;
    term->ptr = term_row_ptr(term, y) + term->cur->x;
    term_clean_line(term, y);
}

// Cursor down
static void term_out_CUD(term_state_t *term)
{
    uint16_t rows = term->csi_param[0];
    if (rows < 1)
        rows = 1;
    // Soft fence: cursor already inside the region cannot cross margin_bot.
    uint8_t ceil = (term->cur->y <= term->screen->margin_bot) ? term->screen->margin_bot
                                                              : (uint8_t)(term->height - 1);
    uint16_t y = term->cur->y;
    while (rows && y < ceil)
        --rows, ++y;
    term->cur->y = y;
    term->ptr = term_row_ptr(term, y) + term->cur->x;
    term_clean_line(term, y);
}

// Cursor forward
static void term_out_CUF(term_state_t *term)
{
    uint16_t cols = term->csi_param[0];
    if (cols < 1)
        cols = 1;
    if (cols > term->width * term->height)
        cols = term->width * term->height;
    if (cols > term->width - term->cur->x)
    {
        if (term->screen->wrapped[term->cur->y])
        {
            term->csi_param[0] = cols - (term->width - term->cur->x);
            term_out_CR(term);
            term_out_LF(term, true);
            return term_out_CUF(term);
        }
        else
            cols = term->width - term->cur->x;
    }
    term->ptr += cols;
    term->cur->x += cols;
}

// Cursor backward
static void term_out_CUB(term_state_t *term)
{
    uint16_t cols = term->csi_param[0];
    if (cols < 1)
        cols = 1;
    if (cols > term->width * term->height)
        cols = term->width * term->height;

    if (cols > term->cur->x)
    {
        if (term->cur->y && term->screen->wrapped[term->cur->y - 1])
        {
            term->csi_param[0] = cols - term->cur->x;
            term->cur->y--;
            term->cur->x = term->width;
            term->ptr = term_row_ptr(term, term->cur->y) + term->cur->x;
            return term_out_CUB(term);
        }
        else
            cols = term->cur->x;
    }
    term->ptr -= cols;
    term->cur->x -= cols;
}

// Delete characters
static void term_out_DCH(term_state_t *term)
{
    unsigned max_chars = term->width - term->cur->x;
    for (uint8_t i = term->cur->y; i < term->height - 1 && term->screen->wrapped[i]; i++)
        max_chars += term->width;
    uint16_t chars = term->csi_param[0];
    if (chars < 1)
        chars = 1;
    if (chars > max_chars)
        chars = max_chars;

    // Walk logically: physical rows are not adjacent after a DECSTBM scroll,
    // so refresh row pointers at every row boundary instead of stepping a
    // single ptr through memory.
    uint8_t y_dst = term->cur->y;
    uint8_t x_dst = term->cur->x;
    term_data_t *row_dst = term_row_ptr(term, y_dst);
    uint8_t y_src = term->cur->y;
    uint16_t x_src_raw = (uint16_t)term->cur->x + chars;
    while (x_src_raw >= term->width)
    {
        x_src_raw -= term->width;
        y_src++;
    }
    uint8_t x_src = (uint8_t)x_src_raw;
    term_data_t *row_src = term_row_ptr(term, y_src);

    for (unsigned i = 0; i < max_chars - chars; i++)
    {
        row_dst[x_dst] = row_src[x_src];
        if (++x_dst == term->width)
        {
            x_dst = 0;
            y_dst++;
            row_dst = term_row_ptr(term, y_dst);
        }
        if (++x_src == term->width)
        {
            x_src = 0;
            y_src++;
            row_src = term_row_ptr(term, y_src);
        }
    }
    uint16_t dch_fg, dch_bg;
    uint8_t dch_attr;
    term_emit_attrs(term, &dch_fg, &dch_bg, &dch_attr);
    for (unsigned i = max_chars - chars; i < max_chars; i++)
    {
        row_dst[x_dst].font_code = ' ';
        row_dst[x_dst].fg_color = dch_fg;
        row_dst[x_dst].bg_color = dch_bg;
        row_dst[x_dst].attributes = dch_attr;
        if (++x_dst == term->width)
        {
            x_dst = 0;
            y_dst++;
            row_dst = term_row_ptr(term, y_dst);
        }
    }

    // Scan back from the end of the logical line to find the real content
    // length, then retire any wrap-chain rows the content no longer reaches.
    unsigned content_len = max_chars;
    uint8_t y_scan = term->cur->y;
    uint16_t x_scan_raw = (uint16_t)term->cur->x + max_chars;
    while (x_scan_raw >= term->width)
    {
        x_scan_raw -= term->width;
        y_scan++;
    }
    uint8_t x_scan = (uint8_t)x_scan_raw;
    term_data_t *row_scan = term_row_ptr(term, y_scan);
    while (content_len > 0)
    {
        if (x_scan == 0)
        {
            y_scan--;
            x_scan = term->width - 1;
            row_scan = term_row_ptr(term, y_scan);
        }
        else
            x_scan--;
        if (row_scan[x_scan].font_code != ' ')
            break;
        content_len--;
    }
    unsigned row_capacity = term->width - term->cur->x;
    for (uint8_t i = term->cur->y; i < term->height - 1 && term->screen->wrapped[i]; i++)
    {
        if (content_len <= row_capacity)
        {
            for (uint8_t j = i; j < term->height - 1 && term->screen->wrapped[j]; j++)
                term->screen->wrapped[j] = false;
            break;
        }
        content_len -= row_capacity;
        row_capacity = term->width;
    }
}

// Insert Pn blank characters at the cursor. Walks the wrap chain inverted
// to DCH so content shifts right across all wrapped successor rows; the
// chain length is unchanged (content overflowing past chain end is lost).
static void term_out_ICH(term_state_t *term)
{
    unsigned max_chars = term->width - term->cur->x;
    for (uint8_t i = term->cur->y; i < term->height - 1 && term->screen->wrapped[i]; i++)
        max_chars += term->width;
    uint16_t chars = term->csi_param[0];
    if (chars < 1)
        chars = 1;
    if (chars > max_chars)
        chars = max_chars;

    // Shift right by `chars`, walking source high-to-low so we never
    // clobber a source cell before reading it.
    if (max_chars > chars)
    {
        unsigned copy_count = (unsigned)(max_chars - chars);

        // Position dst at offset (max_chars - 1).
        uint8_t y_dst = term->cur->y;
        uint16_t off = (uint16_t)(term->cur->x + (max_chars - 1));
        while (off >= term->width)
        {
            off -= term->width;
            y_dst++;
        }
        uint8_t x_dst = (uint8_t)off;
        term_data_t *row_dst = term_row_ptr(term, y_dst);

        // Position src at offset (max_chars - 1 - chars).
        uint8_t y_src = term->cur->y;
        off = (uint16_t)(term->cur->x + (max_chars - 1 - chars));
        while (off >= term->width)
        {
            off -= term->width;
            y_src++;
        }
        uint8_t x_src = (uint8_t)off;
        term_data_t *row_src = term_row_ptr(term, y_src);

        for (unsigned i = 0; i < copy_count; i++)
        {
            row_dst[x_dst] = row_src[x_src];
            if (x_dst == 0)
            {
                x_dst = (uint8_t)(term->width - 1);
                y_dst--;
                row_dst = term_row_ptr(term, y_dst);
            }
            else
                x_dst--;
            if (x_src == 0)
            {
                x_src = (uint8_t)(term->width - 1);
                y_src--;
                row_src = term_row_ptr(term, y_src);
            }
            else
                x_src--;
        }
    }

    uint16_t fg, bg;
    uint8_t attr;
    term_emit_attrs(term, &fg, &bg, &attr);
    uint8_t y_fill = term->cur->y;
    uint8_t x_fill = term->cur->x;
    term_data_t *row_fill = term_row_ptr(term, y_fill);
    for (unsigned i = 0; i < chars; i++)
    {
        row_fill[x_fill].font_code = ' ';
        row_fill[x_fill].fg_color = fg;
        row_fill[x_fill].bg_color = bg;
        row_fill[x_fill].attributes = attr;
        if (++x_fill == term->width)
        {
            x_fill = 0;
            y_fill++;
            if (y_fill < term->height)
                row_fill = term_row_ptr(term, y_fill);
        }
    }
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
    uint16_t fg, bg;
    uint8_t attr;
    term_emit_attrs(term, &fg, &bg, &attr);
    term_data_t *row = term_row_ptr(term, term->cur->y);
    for (uint16_t i = 0; i < n; i++)
    {
        row[term->cur->x + i].font_code = ' ';
        row[term->cur->x + i].fg_color = fg;
        row[term->cur->x + i].bg_color = bg;
        row[term->cur->x + i].attributes = attr;
    }
}

// Scroll Up Pn rows within the current scroll region. Cursor unchanged.
static void term_out_SU(term_state_t *term)
{
    uint16_t n = term->csi_param[0];
    if (n < 1)
        n = 1;
    term_region_scroll_up(term, term->screen->margin_top, term->screen->margin_bot, (uint8_t)n);
    term_clean_line(term, term->cur->y);
}

// Scroll Down Pn rows within the current scroll region. Cursor unchanged.
static void term_out_SD(term_state_t *term)
{
    uint16_t n = term->csi_param[0];
    if (n < 1)
        n = 1;
    term_region_scroll_down(term, term->screen->margin_top, term->screen->margin_bot, (uint8_t)n);
    term_clean_line(term, term->cur->y);
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
// 3/4 = underline, 5/6 = bar. Stored but the software-rendered block-invert
// cursor doesn't yet honor the shape.
static void term_out_DECSCUSR(term_state_t *term)
{
    uint16_t ps = term->csi_param[0];
    if (ps > 6)
        ps = 0;
    term->cursor_style = (uint8_t)ps;
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

    if (term->cur->origin_mode)
    {
        // Row is relative to margin_top; clamp inside the region.
        uint8_t region_h = term->screen->margin_bot - term->screen->margin_top + 1;
        if (row > region_h)
            row = region_h;
        row += term->screen->margin_top;
    }
    else if (row > term->height)
        row = term->height;

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
    if (term->cur->origin_mode)
    {
        uint8_t region_h = (uint8_t)(term->screen->margin_bot - term->screen->margin_top + 1);
        if (row > region_h)
            row = region_h;
        row += term->screen->margin_top;
    }
    else if (row > term->height)
        row = term->height;
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
    uint16_t erase_fg, erase_bg;
    uint8_t erase_a;
    term_emit_attrs(term, &erase_fg, &erase_bg, &erase_a);
    switch (term->csi_param[0])
    {
    case 0: // to the end of the line
    case 1: // to beginning of the line
    {
        term_data_t *row = term_row_ptr(term, term->cur->y);
        uint8_t x, end;
        if (!term->csi_param[0])
        {
            x = term->cur->x;
            end = term->width - 1;
            // Erasing from cursor to end severs the wrap chain.
            term->screen->wrapped[term->cur->y] = false;
        }
        else
        {
            x = 0;
            end = term->cur->x;
        }
        for (; x <= end; x++)
        {
            row[x].font_code = ' ';
            row[x].fg_color = erase_fg;
            row[x].bg_color = erase_bg;
            row[x].attributes = erase_a;
        }
        break;
    }
    case 2: // full line
        term->screen->wrapped[term->cur->y] = false;
        term->screen->dirty[term->cur->y] = true;
        term->screen->erase_fg_color[term->cur->y] = erase_fg;
        term->screen->erase_bg_color[term->cur->y] = erase_bg;
        term->screen->erase_attr[term->cur->y] = erase_a;
        term_clean_line(term, term->cur->y);
        break;
    }
}

// Erase Display
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
        term->csi_param[0] = 1;
        return term_out_CUB(term);
    case '\t': // HT
        return term_out_HT(term);
    case '\n': // LF
        return term_out_LF(term, false);
    case '\f': // FF
        return term_out_FF(term);
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
        return term_out_glyph(term, ch);
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
        term_out_LF(term, false);
        term->ansi_state = ansi_state_C0;
    }
    else if (ch == 'E') // NEL - Next Line (CR + IND)
    {
        term_out_CR(term);
        term_out_LF(term, false);
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

// ESC # c -- DEC private intermediate. DECALN (ESC # 8) is the only thing
// it carries, and that's a vttest-only full-screen 'E' fill we deliberately
// don't implement -- the 2400-cell write isn't worth supporting a sequence
// no real app emits. The state still exists so the byte after ESC # is
// swallowed instead of printing as a glyph.
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
        // CSI intermediates: only DECSCUSR (SP q) and DECSTR (! p) recognized.
        if (term->csi_intermediate == ' ' && ch == 'q')
            term_out_DECSCUSR(term);
        else if (term->csi_intermediate == '!' && ch == 'p')
            term_out_DECSTR(term);
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
        term_out_CUD(term);
        break;
    case 'C':
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
                term->cursor_enabled = true;
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
            case 1047: /* alt screen, no cursor save, clear-on-exit */
                term_enter_alt(term, false, false);
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
            case 47:
                term_leave_alt(term, false, false);
                break;
            case 1047: /* clear alt screen before swapping back */
                term_leave_alt(term, false, true);
                break;
            case 1049:
                term_leave_alt(term, true, false);
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
        else
            term->csi_param_count = TERM_CSI_PARAM_MAX_LEN;
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
    case ansi_state_CSI_less:
    case ansi_state_CSI_equal:
    case ansi_state_CSI_greater:
        // Private CSI parameter bytes; recognize the sequence so digits
        // don't misparse, then discard. This covers CSI > c (secondary DA)
        // and CSI = c (tertiary DA) -- we deliberately don't reply, since
        // there's no well-defined secondary-DA response for the Linux
        // console subset, and apps that probe accept silence.
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
//   OSC 10 ; #rrggbb   set default foreground
//   OSC 11 ; #rrggbb   set default background
//   OSC 12 ; #rrggbb   set cursor color
//   OSC 110            reset default foreground
//   OSC 111            reset default background
//   OSC 112            reset cursor color
// Spec format restricted to "#rrggbb"; other OSC codes/specs are silently
// ignored for forward compatibility.
static void term_out_OSC(term_state_t *term)
{
    uint8_t count = term->csi_param_count;
    bool spec_ok = (count == 8);
    bool empty = (count == 0);
    uint16_t packed = 0;
    if (spec_ok)
        packed = SCANVIDEO_ALPHA_MASK | SCANVIDEO_PIXEL_FROM_RGB8(
                                            term->csi_param[1],
                                            term->csi_param[2],
                                            term->csi_param[3]);
    switch (term->csi_param[0])
    {
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
            term->cursor_bg_color = packed;
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
            term->cursor_bg_color = color_256[TERM_FG_COLOR_INDEX];
        break;
    }
}

// Streaming OSC body parser; uses csi_param[] as scratch storage.
//   csi_param[0]      = Ps (accumulated digits)
//   csi_param[1..3]   = parsed R, G, B bytes once spec begins
//   csi_param_count   = sub-state cursor:
//       0           collecting Ps digits
//       1           saw ';', expecting '#'
//       2..7        accumulating hex digits of #rrggbb
//       8           spec complete, awaiting terminator
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
            term->csi_param_count = 1;
            term->csi_param[1] = 0;
            term->csi_param[2] = 0;
            term->csi_param[3] = 0;
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
    if (length)
    {
        // Brief dark gap while typing -- next blink tick re-lights to the
        // appropriate steady/blink state.
        term_40.cursor_lit = false;
        term_80.cursor_lit = false;
        for (int i = 0; i < length; i++)
        {
            term_out_char(&term_40, buf[i]);
            term_out_char(&term_80, buf[i]);
        }
        term_40.timer = term_80.timer = make_timeout_time_us(TERM_CURSOR_INPUT_GAP_US);
    }
}

void term_init(void)
{
    // prepare console
    static term_data_t term40_pri_mem[40 * TERM_MAX_HEIGHT];
    static term_data_t term40_alt_mem[40 * TERM_MAX_HEIGHT];
    static term_data_t term80_pri_mem[80 * TERM_MAX_HEIGHT];
    static term_data_t term80_alt_mem[80 * TERM_MAX_HEIGHT];
    term_state_init(&term_40, 40, term40_pri_mem, term40_alt_mem);
    term_state_init(&term_80, 80, term80_pri_mem, term80_alt_mem);
    term_blink_phase = 0;
    term_blink_phase_timer = make_timeout_time_us(TERM_BLINK_PHASE_US);
    // become part of stdout
    static stdio_driver_t term_stdio = {
        .out_chars = com_out_chars,
        .crlf_enabled = true,
    };
    stdio_set_driver_enabled(&term_stdio, true);
}

// Drives cursor_lit based on cursor_style. Steady styles (2/4/6) re-light
// after the input-cooldown gap and park for a long idle interval; blinking
// styles (0/1/3/5) toggle on the usual cadence. Disabled cursors are forced
// unlit so the renderer never overlays them.
static void term_blink_cursor(term_state_t *term)
{
    if (!term->cursor_enabled)
    {
        term->cursor_lit = false;
        return;
    }
    if (!time_reached(term->timer))
        return;
    bool steady = (term->cursor_style == 2 ||
                   term->cursor_style == 4 ||
                   term->cursor_style == 6);
    if (steady)
    {
        term->cursor_lit = true;
        term->timer = make_timeout_time_ms(TERM_CURSOR_STEADY_IDLE_MS);
        return;
    }
    term->cursor_lit = !term->cursor_lit;
    // Fast blink when off right side (deferred-wrap state).
    if (term->cur->x == term->width)
        term->timer = make_timeout_time_us(TERM_CURSOR_BLINK_FAST_US);
    else
        term->timer = make_timeout_time_us(TERM_CURSOR_BLINK_US);
}

static void term_blink_phase_task(void)
{
    if (time_reached(term_blink_phase_timer))
    {
        term_blink_phase = term_blink_phase ? 0u : ATTR_BLINK;
        term_blink_phase_timer = make_timeout_time_us(TERM_BLINK_PHASE_US);
    }
}

void term_task(void)
{
    term_blink_cursor(&term_40);
    term_blink_cursor(&term_80);
    term_blink_phase_task();
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
    // blink_mask is the global phase byte (0 or ATTR_BLINK).
    const uint8_t blink_mask = term_blink_phase;
    const uint8_t line_mask =
        (uint8_t)((scanrow == 7 ? ATTR_UNDERLINE : 0) |
                  ((scanrow == 7 || scanrow == 5) ? ATTR_DBL_UL : 0) |
                  (scanrow == 4 ? ATTR_STRIKE : 0) |
                  (scanrow == 0 ? ATTR_OVERLINE : 0));
    const uint8_t row_idx = (uint8_t)(scanline_id / 8);
    term_data_t *term_ptr = term_row_ptr(&term_40, row_idx);
    uint16_t *const rgb_line = rgb;
    for (int i = 0; i < 40; i++, term_ptr++)
    {
        uint8_t attr = term_ptr->attributes;
        uint8_t bits = font_line[term_ptr->font_code];
        uint16_t fg = term_ptr->fg_color;
        uint16_t bg = term_ptr->bg_color;
        if (attr)
        {
            if (attr & ATTR_DEC)
                bits = font_line_dec[(uint8_t)(term_ptr->font_code - 0x60)];
            if (attr & blink_mask)
                fg = bg;
            if (attr & line_mask)
                bits = 0xFF;
        }
        modes_render_1bpp(rgb, bits, bg, fg);
        rgb += 8;
    }
    // Cursor overlay: at most one cell per scanline. Patches the rendered
    // pixels in place; cursor wins over ATTR_BLINK on its cell.
    if (term_40.cursor_enabled && term_40.cursor_lit &&
        row_idx == term_40.cur->y)
    {
        uint8_t cx = term_40.cur->x;
        if (cx >= term_40.width)
            cx = (uint8_t)(term_40.width - 1);
        uint16_t *crgb = rgb_line + (uint32_t)cx * 8;
        const uint16_t cursor_color = term_40.cursor_bg_color;
        switch (term_40.cursor_style)
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
            term_data_t *cp = term_row_ptr(&term_40, row_idx) + cx;
            uint8_t cattr = cp->attributes;
            uint8_t cbits = font_line[cp->font_code];
            if (cattr & ATTR_DEC)
                cbits = font_line_dec[(uint8_t)(cp->font_code - 0x60)];
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
    const uint8_t blink_mask = term_blink_phase;
    const uint8_t line_mask =
        (uint8_t)((scanrow == 15 ? ATTR_UNDERLINE : 0) |
                  ((scanrow == 15 || scanrow == 13) ? ATTR_DBL_UL : 0) |
                  (scanrow == 8 ? ATTR_STRIKE : 0) |
                  (scanrow == 0 ? ATTR_OVERLINE : 0));
    const uint8_t row_idx = (uint8_t)(scanline_id / 16);
    term_data_t *term_ptr = term_row_ptr(&term_80, row_idx);
    uint16_t *const rgb_line = rgb;
    for (int i = 0; i < 80; i++, term_ptr++)
    {
        uint8_t attr = term_ptr->attributes;
        uint8_t bits = font_line[term_ptr->font_code];
        uint16_t fg = term_ptr->fg_color;
        uint16_t bg = term_ptr->bg_color;
        if (attr)
        {
            if (attr & ATTR_DEC)
                bits = font_line_dec[(uint8_t)(term_ptr->font_code - 0x60)];
            else if ((attr & ATTR_ITALIC) && term_ptr->font_code < 0x80)
                bits = italic_line[term_ptr->font_code];
            if (attr & blink_mask)
                fg = bg;
            if (attr & line_mask)
                bits = 0xFF;
        }
        modes_render_1bpp(rgb, bits, bg, fg);
        rgb += 8;
    }
    // Cursor overlay: at most one cell per scanline. Underline strip is the
    // bottom 2 rows on 8x16; bar is 2px wide for proportionality.
    if (term_80.cursor_enabled && term_80.cursor_lit &&
        row_idx == term_80.cur->y)
    {
        uint8_t cx = term_80.cur->x;
        if (cx >= term_80.width)
            cx = (uint8_t)(term_80.width - 1);
        uint16_t *crgb = rgb_line + (uint32_t)cx * 8;
        const uint16_t cursor_color = term_80.cursor_bg_color;
        switch (term_80.cursor_style)
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
            term_data_t *cp = term_row_ptr(&term_80, row_idx) + cx;
            uint8_t cattr = cp->attributes;
            uint8_t cbits = font_line[cp->font_code];
            if (cattr & ATTR_DEC)
                cbits = font_line_dec[(uint8_t)(cp->font_code - 0x60)];
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
