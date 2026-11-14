/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/term/cells.h"
#include "core/term/grid.h"
#include "core/term/term.h"
#include "core/vga/pixel_format.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

// A VT102 with most of what xterm and the Linux console add: 256 colors and
// truecolor, the alternate screen, the dynamic colors of OSC 4/10/11/12, the
// Linux palette sequences and SGR 10/11. The parser follows the DEC/xterm
// state machine (vt100.net), so unknown sequences and strings are taken apart
// the way xterm does. Inside a sequence BS-SI and BEL act and the sequence
// goes on, as in xterm; any other control ends it silently, as in Linux. DEC
// Special Graphics is a character set (ESC ( 0 / SO / SI) the cells draw
// from. Bytes 0x80-0xFF are always glyphs of the code page: there are no C1
// controls and no UTF-8.
//
// This is the parser and everything that is the same at every size: the
// modes, the pens, the character sets and the queries. What moves the cursor
// or the rows it hands to each grid (grid.c).

/* The longest answer the terminal builds for a query. Replies snprintf into a
 * buffer this size and drop the reply if it would not fit, so this bounds
 * what a program can be told, not what the console can hold. */
#define TERM_REPLY_MAX 24

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

/* ---- the grids --------------------------------------------------------- */

// Hand what moves the cursor or the rows to every grid.
static void term_grids(term_t *t, grid_op_t op, uint8_t ch)
{
    for (uint8_t i = 0; i < t->grid_count; i++)
        grid_do(t, t->grids[i], op, ch);
}

// Show the primary or the alternate screen: the pen, and each grid's cursor
// and line sizes, are that screen's from now on.
static void term_show(term_t *t, bool alt)
{
    t->alt_active = alt;
    t->cur = &t->pen[alt];
    for (uint8_t i = 0; i < t->grid_count; i++)
    {
        term_grid_t *g = t->grids[i];
        g->cur = &g->pos[alt];
        g->ls = g->lines + (alt ? g->rows : 0);
    }
}

/* ---- resets ------------------------------------------------------------ */

// The power-up pen: SGR 0, ASCII in G0 and G1, origin mode off. It is also
// what DECRC restores when nothing was saved.
static void term_default_pen(term_pen_t *pen)
{
    memset(pen, 0, sizeof *pen);
}

static void term_reset_parser(term_t *t)
{
    t->ansi_state = ansi_state_ground;
    t->csi_intermediate = 0;
    t->osc_pending = false;
    t->rep_valid = false;
}

// Everything RIS and the system's soft reset share: every mode and setting,
// the colors, the tab stops, the margins, both pens and both saved cursors,
// and the blinks restarted in step. The alternate screen is dropped and
// erased. Only the size, the code page, the primary screen (its cells and
// line sizes) and the cursor's position are left. New state added later
// joins this list.
static void term_reset_core(term_t *t)
{
    term_show(t, false);
    t->autowrap = true;
    t->insert_mode = false;
    t->newline_mode = false;
    t->disp_ctrl = false;
    t->vt52 = false;
    t->ice_colors = false;
    t->reverse_screen = false;
    t->cursor_enabled = true;
    t->cursor_style = 0;
    term_reset_parser(t);
    for (int s = 0; s < 2; s++)
    {
        term_default_pen(&t->pen[s]);
        term_default_pen(&t->saved[s]);
        t->has_saved[s] = false;
    }
    cells_colors_reset(t);
    term_grids(t, GRID_RESET, 0);
}

void term_RIS(term_t *t)
{
    term_reset_core(t);
    term_grids(t, GRID_CLEAR, 0);
    term_grids(t, GRID_HOME, 0);
}

void term_RIS_no_clear(term_t *t)
{
    term_reset_core(t);
    term_grids(t, GRID_STAY, 0);
}

// DECSTR, CSI ! p: the soft reset a program sends. It resets the modes on
// DEC's list -- IRM, DECOM, DECAWM (to our default, on), DECTCEM, the
// character sets, SGR, the margins and the saved cursor -- and ends a pending
// wrap, but leaves the cursor where it is. The cursor style, iCE colors, the
// palette, the tab stops and the screen are left alone.
static void term_out_DECSTR(term_t *t)
{
    t->insert_mode = false;
    t->autowrap = true;
    t->cursor_enabled = true;
    term_default_pen(term_pen(t));
    for (int s = 0; s < 2; s++)
    {
        term_default_pen(&t->saved[s]);
        t->has_saved[s] = false;
    }
    term_grids(t, GRID_DECSTR, 0);
}

term_grid_t *term_shown(term_t *t)
{
    return t->grid_count > TERM_SCREEN_GRIDS ? t->grids[TERM_SCREEN_GRIDS] : cells_shown(t);
}

void term_add_grid(term_t *t, term_grid_t *g, const term_grid_t *from,
                   uint8_t *lines, uint8_t rows)
{
    assert(t->grid_count < TERM_MAX_GRIDS);
    term_grid_copy(g, from, lines, rows);
    t->grids[t->grid_count++] = g;
    term_show(t, t->alt_active);
}

void term_drop_grid(term_t *t, term_grid_t *g)
{
    for (uint8_t i = 0; i < t->grid_count; i++)
        if (t->grids[i] == g)
        {
            for (; i + 1 < t->grid_count; i++)
                t->grids[i] = t->grids[i + 1];
            t->grid_count--;
            return;
        }
}

void term_init(term_t *t, term_grid_t *const *grids, uint8_t count)
{
    assert(count <= TERM_MAX_GRIDS);
    memset(t, 0, sizeof *t);
    for (uint8_t i = 0; i < count; i++)
        t->grids[i] = grids[i];
    t->grid_count = count;
    term_RIS(t);
}

/* ---- saved cursor and screens ------------------------------------------ */

// DECSC (ESC 7, CSI s, ?1048 h): save the cursor, its pen, the pending wrap
// and the character sets in the showing screen's slot. Each screen has its
// own.
static void term_save_cursor(term_t *t)
{
    t->saved[t->alt_active] = *term_pen(t);
    t->has_saved[t->alt_active] = true;
    term_grids(t, GRID_SAVE, 0);
}

// DECRC (ESC 8, CSI u, ?1048 l): restore them. With nothing saved the slot
// holds the defaults: home, SGR 0, ASCII, origin mode off.
static void term_restore_cursor(term_t *t)
{
    *term_pen(t) = t->saved[t->alt_active];
    term_grids(t, GRID_RESTORE, 0);
}

// Enter the alternate screen. The cursor and its pen come along, as in xterm,
// where they belong to the terminal and not to a screen. ?1049 clears it.
static void term_enter_alt(term_t *t, bool clear)
{
#if TERM_ALT_SCREEN
    if (!t->alt_active)
    {
        t->pen[1] = t->pen[0];
        term_grids(t, GRID_ALT_IN, 0);
        term_show(t, true);
    }
    if (clear)
        term_grids(t, GRID_CLEAR, 0);
    term_grids(t, GRID_STAY, 0);
#else
    /* No second screen on this machine: a full-screen program draws on the
     * primary and leaves its output behind, as on a terminal without one. */
    (void)t;
    (void)clear;
#endif
}

// Leave the alternate screen, the cursor coming back along with it. ?1047
// clears the alternate screen first; ?1049 restores the cursor instead.
static void term_leave_alt(term_t *t, bool clear, bool restore)
{
#if TERM_ALT_SCREEN
    if (!t->alt_active)
    {
        // ?1049 l on the primary screen still restores the cursor, as in
        // xterm.
        if (restore)
            term_restore_cursor(t);
        return;
    }
    if (clear)
        term_grids(t, GRID_CLEAR, 0);
    if (!restore)
    {
        t->pen[0] = t->pen[1];
        term_grids(t, GRID_ALT_OUT, 0);
    }
    term_show(t, false);
#else
    (void)clear;
#endif
    if (restore)
        term_restore_cursor(t);
    else
        term_grids(t, GRID_STAY, 0);
}

/* ---- SGR --------------------------------------------------------------- */

// Read the color of SGR 38, 48 or 58 starting at param i, with `subs` colon
// sub-parameters after it. Returns how many params after i were taken; kind
// is TERM_COLOR_DEFAULT when they named no color, in which case the pen keeps
// its color, and TERM_COLOR_CLEAR for 38;1.
//   38;5;N  38:5:N            palette N; nothing for N > 255
//   38;2;r;g;b  38:2::r:g:b  38:2:r:g:b   truecolor; nothing for a channel > 255
//   38;1  38:1               transparent (ours)
// An unknown or cut-off form is taken whole, so its numbers are never run as
// SGR codes of their own.
static uint8_t term_sgr_color(const term_t *t, uint8_t i, uint8_t subs,
                              uint8_t *kind, uint16_t *value)
{
    const uint16_t *p = t->csi_param;
    uint8_t avail = subs ? subs : (uint8_t)(t->csi_param_count - 1 - i);
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
        *kind = TERM_COLOR_CLEAR;
        break;
    }
    if (subs)
        return subs;
    return want < avail ? want : avail;
}

static void term_sgr_reset(term_pen_t *cur)
{
    cur->bold = false;
    cur->faint = false;
    cur->reverse = false;
    cur->conceal = false;
    cur->sgr_attr = 0;
    cur->fg.kind = TERM_COLOR_DEFAULT;
    cur->bg.kind = TERM_COLOR_DEFAULT;
    memset(&cur->ul, 0, sizeof cur->ul);
    cur->ul_set = false;
}

static void term_out_SGR(term_t *t)
{
    term_pen_t *cur = term_pen(t);
    uint8_t count = t->csi_param_count;
    for (uint8_t i = 0; i < count;)
    {
        uint16_t param = t->csi_param[i];
        // The colon sub-parameters that belong to this one.
        uint8_t subs = 0;
        while (i + subs + 1 < count && t->csi_separator[i + subs] == ':')
            subs++;
        uint8_t used = subs;
        switch (param)
        {
        case 0: // reset
            term_sgr_reset(cur);
            break;
        case 1: // bold intensity
            cur->bold = true;
            break;
        case 2: // faint: each channel halved
            cur->faint = true;
            break;
        case 3: // italic (renders only in 8x16 mode; 8x8 has no italic font)
            cur->sgr_attr |= TERM_ATTR_ITALIC;
            break;
        case 4: // underline; kitty's 4:n picks a style
        {
            uint16_t style = subs ? t->csi_param[i + 1] : 1;
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
            t->disp_ctrl = false;
            break;
        case 11: // first alternate font (SCO, Linux): controls are drawn
            t->disp_ctrl = true;
            break;
        case 21: // double underline (ECMA-48, xterm)
            cur->sgr_attr |= TERM_ATTR_DBL_UL;
            break;
        case 22: // normal intensity: cancels both bold and faint
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
            cur->fg.kind = TERM_COLOR_INDEX;
            cur->fg.index = (uint8_t)(param - 30);
            break;
        case 38:
        case 48:
        case 58:
        {
            uint8_t kind;
            uint16_t value = 0;
            used = term_sgr_color(t, i, subs, &kind, &value);
            if (kind == TERM_COLOR_DEFAULT)
                break;
            term_color_t *dst = param == 38 ? &cur->fg : param == 48 ? &cur->bg : &cur->ul;
            if (kind == TERM_COLOR_CLEAR)
            {
                // SGR 38;1: the color the pen has, made transparent. An
                // underline that follows the foreground takes its color.
                if (param == 58 && !cur->ul_set)
                    cur->ul = cur->fg;
                dst->kind |= TERM_COLOR_CLEAR;
            }
            else
            {
                dst->kind = kind;
                dst->index = (uint8_t)value;
                dst->rgb = value;
            }
            if (param == 58)
                cur->ul_set = true;
            break;
        }
        case 39: // default foreground
            cur->fg.kind = TERM_COLOR_DEFAULT;
            break;
        case 40:
        case 41:
        case 42:
        case 43: // background color
        case 44:
        case 45:
        case 46:
        case 47:
            cur->bg.kind = TERM_COLOR_INDEX;
            cur->bg.index = (uint8_t)(param - 40);
            break;
        case 49: // default background
            cur->bg.kind = TERM_COLOR_DEFAULT;
            break;
        case 53: // overline
            cur->sgr_attr |= TERM_ATTR_OVERLINE;
            break;
        case 55: // overline off
            cur->sgr_attr &= (uint8_t)~TERM_ATTR_OVERLINE;
            break;
        case 59: // default underline color
            cur->ul_set = false;
            break;
        case 90:
        case 91:
        case 92:
        case 93: // bright foreground color
        case 94:
        case 95:
        case 96:
        case 97:
            // The bright palette entry itself, so SGR 22 leaves it bright,
            // as xterm does.
            cur->fg.kind = TERM_COLOR_INDEX;
            cur->fg.index = (uint8_t)(param - 90 + 8);
            break;
        case 100:
        case 101:
        case 102:
        case 103: // bright background color
        case 104:
        case 105:
        case 106:
        case 107:
            cur->bg.kind = TERM_COLOR_INDEX;
            cur->bg.index = (uint8_t)(param - 100 + 8);
            break;
        }
        i = (uint8_t)(i + 1 + used);
    }
}

/* ---- replies ----------------------------------------------------------- */

static void term_replyf(const term_t *t, const char *fmt, unsigned a,
                        unsigned b, unsigned c)
{
    char buf[TERM_REPLY_MAX];
    int n = snprintf(buf, sizeof(buf), fmt, a, b, c);
    if (n > 0 && n < (int)sizeof(buf))
        cells_reply(t, buf, (size_t)n);
}

#define TERM_REPLY(t, lit) cells_reply((t), (lit), sizeof(lit) - 1)

// Device Status Report: 5 status, 6 cursor position.
static void term_out_DSR(term_t *t)
{
    switch (t->csi_param[0])
    {
    case 5:
        TERM_REPLY(t, "\33[0n");
        break;
    case 6:
    {
        const term_grid_t *g = term_shown(t);
        const term_pos_t *pos = &g->pos[t->alt_active];
        unsigned y = pos->y;
        // Region-relative under DECOM, guarded so a cursor above the region
        // reports row 1 rather than underflowing.
        if (term_pen(t)->origin_mode)
            y = (y >= g->margin_top) ? (y - g->margin_top) : 0;
        term_replyf(t, "\33[%u;%uR", y + 1, pos->x + 1u, 0);
        break;
    }
    }
}

// xterm's window reports, CSI Ps t, of the ones that mean something here.
// Linux answers none of them.
static void term_out_window(term_t *t)
{
    if (t->csi_param_count > 1)
        return;
    const term_grid_t *g = term_shown(t);
    // The cell is the screen's font, whatever size the program set.
    unsigned cell = term_cell_height(cells_shown(t));
    switch (t->csi_param[0])
    {
    case 11: // iconified? never
        TERM_REPLY(t, "\33[1t");
        break;
    case 14: // text area in pixels
        term_replyf(t, "\33[4;%u;%ut", g->height * cell, g->width * 8u, 0);
        break;
    case 16: // character cell in pixels
        term_replyf(t, "\33[6;%u;%ut", cell, 8, 0);
        break;
    case 18: // text area in characters
        term_replyf(t, "\33[8;%u;%ut", g->height, g->width, 0);
        break;
    case 19: // screen in characters
        term_replyf(t, "\33[9;%u;%ut", g->height, g->width, 0);
        break;
    }
}

// Request Mode (DECRQM). Reply: CSI ? Ps ; Pm $ y (private) or CSI Ps ; Pm $ y
// (ANSI). Pm: 0 not recognized, 1 set, 2 reset, 3 permanently set,
// 4 permanently reset.
static void term_out_DECRQM(term_t *t, bool private)
{
    unsigned ps = t->csi_param[0];
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
        case 5: // DECSCNM
            pm = t->reverse_screen ? 1 : 2;
            break;
        case 6: // DECOM
            pm = term_pen(t)->origin_mode ? 1 : 2;
            break;
        case 7: // DECAWM
            pm = t->autowrap ? 1 : 2;
            break;
        case 12: // cursor blink
            pm = (t->cursor_style == 2 || t->cursor_style == 4 ||
                  t->cursor_style == 6)
                     ? 2
                     : 1;
            break;
        case 25: // DECTCEM
            pm = t->cursor_enabled ? 1 : 2;
            break;
        case 33: // iCE colors
            pm = t->ice_colors ? 1 : 2;
            break;
        case 47:
        case 1047:
        case 1049:
            pm = t->alt_active ? 1 : 2;
            break;
        case 1048:
            pm = t->has_saved[t->alt_active] ? 1 : 2;
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
            pm = t->disp_ctrl ? 1 : 2;
            break;
        case 4: // IRM
            pm = t->insert_mode ? 1 : 2;
            break;
        case 12: // SRM: no local echo
            pm = 3;
            break;
        case 20: // LNM
            pm = t->newline_mode ? 1 : 2;
            break;
        }
    }
    term_replyf(t, private ? "\33[?%u;%u$y" : "\33[%u;%u$y", ps, pm, 0);
}

// DECREQTPARM, CSI 0 x or CSI 1 x: the VT100's line settings, answered as a
// VT100 on its fastest line does -- no parity, 8 bits, 9600 baud both ways, clock
// multiplier 1, no flags. CSI 0 x asks for a report unsolicited reports may
// follow (2); CSI 1 x, for one only on request (3).
static void term_out_DECREQTPARM(term_t *t)
{
    uint16_t ps = t->csi_param[0];
    if (ps <= 1)
        term_replyf(t, "\33[%u;1;1;112;112;1;0x", ps + 2u, 0, 0);
}

/* ---- modes ------------------------------------------------------------- */

// ?12 turns the cursor's blink on or off, as xterm does. It shares the
// DECSCUSR style: 2, 4 and 6 are the steady styles.
static void term_set_cursor_blink(term_t *t, bool blink)
{
    uint8_t s = t->cursor_style;
    if (blink && (s == 2 || s == 4 || s == 6))
        s--;
    else if (!blink && s == 0)
        s = 2;
    else if (!blink && (s & 1))
        s++;
    t->cursor_style = s;
}

// Set cursor style (CSI Ps SP q -- DECSCUSR). 0 = host default, 1/2 = block,
// 3/4 = underline, 5/6 = bar. Values past 6 are ignored.
static void term_out_DECSCUSR(term_t *t)
{
    uint16_t ps = t->csi_param[0];
    if (ps <= 6)
        t->cursor_style = (uint8_t)ps;
}

// SM and RM, the ANSI modes.
static void term_out_SM_RM(term_t *t, bool set)
{
    for (uint8_t i = 0; i < t->csi_param_count; i++)
    {
        switch (t->csi_param[i])
        {
        case 3: // CRM: draw the controls, as SGR 11
            t->disp_ctrl = set;
            break;
        case 4: // IRM: insert mode
            t->insert_mode = set;
            break;
        case 20: // LNM: new line mode
            t->newline_mode = set;
            break;
            // KAM (2) and SRM (12) are taken and ignored.
        }
    }
}

// DECSET and DECRST, the private modes.
static void term_out_DECSET(term_t *t, bool set)
{
    for (uint8_t i = 0; i < t->csi_param_count; i++)
    {
        switch (t->csi_param[i])
        {
        case 2: // DECANM: reset enters VT52 mode; ESC < leaves it
            if (!set)
            {
                t->vt52 = true;
                term_pen(t)->g0_charset = TERM_CS_ASCII;
                term_pen(t)->g1_charset = TERM_CS_ASCII;
                term_pen(t)->gl_is_g1 = false;
            }
            break;
        case 5: // DECSCNM: the whole screen in reverse video
            t->reverse_screen = set;
            break;
        case 6: // DECOM
            term_pen(t)->origin_mode = set;
            term_grids(t, GRID_HOME, 0);
            break;
        case 7: // DECAWM (autowrap)
            t->autowrap = set;
            break;
        case 12: // cursor blink (AT&T 610)
            term_set_cursor_blink(t, set);
            break;
        case 25: // DECTCEM
            t->cursor_enabled = set;
            break;
        case 33: /* iCE colors -- SGR 5/6 becomes the IBM-VGA bright-bg hack */
            t->ice_colors = set;
            break;
        case 47: /* the alternate screen, swap only */
            if (set)
                term_enter_alt(t, false);
            else
                term_leave_alt(t, false, false);
            break;
        case 1047: /* the alternate screen, cleared on leaving */
            if (set)
                term_enter_alt(t, false);
            else
                term_leave_alt(t, true, false);
            break;
        case 1048: /* DECSC and DECRC */
            if (set)
                term_save_cursor(t);
            else
                term_restore_cursor(t);
            break;
        case 1049: /* DECSC, then the alternate screen cleared (the modern default) */
            if (set)
            {
                term_save_cursor(t);
                term_enter_alt(t, true);
            }
            else
                term_leave_alt(t, false, true);
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
static void term_osc_item(term_t *t)
{
    unsigned len = t->osc_len;
    if (len > TERM_OSC_SPEC_MAX)
        len = 0; // too long: no item matches an empty one
    const char *s = t->osc_spec;
    uint16_t color;
    switch (t->osc_ps)
    {
    case 4:
        if (!(t->osc_item & 1))
        {
            int idx = term_parse_index(s, len);
            t->osc_index = idx < 0 ? 0xFFFF : (uint16_t)idx;
        }
        else if (t->osc_index < 256 && term_parse_color(s, len, &color))
            cells_color(t, t->osc_index, color);
        break;
    case 104:
    {
        int idx = term_parse_index(s, len);
        if (idx >= 0)
            cells_color_reset(t, (unsigned)idx);
        break;
    }
    case 10:
    case 11:
    case 12:
        if (!term_parse_color(s, len, &color))
            break;
        switch (t->osc_ps + t->osc_item)
        {
        case 10:
            cells_color(t, CELLS_COLOR_FG, color);
            break;
        case 11:
            cells_color(t, CELLS_COLOR_BG, color);
            break;
        case 12:
            cells_color(t, CELLS_COLOR_CURSOR, color);
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
static void term_osc_end(term_t *t)
{
    t->rep_valid = false;
    if (t->osc_stage == 1)
        term_osc_item(t);
    else if (t->osc_stage == 0)
    {
        switch (t->osc_ps)
        {
        case 104:
            for (unsigned i = 0; i < 256; i++)
                cells_color_reset(t, i);
            break;
        case 110:
            cells_color_reset(t, CELLS_COLOR_FG);
            break;
        case 111:
            cells_color_reset(t, CELLS_COLOR_BG);
            break;
        case 112:
            cells_color_reset(t, CELLS_COLOR_CURSOR);
            break;
        }
    }
}

static void term_osc_byte(term_t *t, uint8_t ch)
{
    if (ch == '\a')
    {
        t->ansi_state = ansi_state_ground;
        term_osc_end(t);
        return;
    }
    if (ch < 0x20)
        return; // other controls inside a string are ignored
    switch (t->osc_stage)
    {
    case 0: // the command number
        if (ch >= '0' && ch <= '9')
        {
            uint32_t v = t->osc_ps * 10u + (ch - '0');
            t->osc_ps = v > 0xFFFF ? 0xFFFF : (uint16_t)v;
            t->osc_len = 1;
        }
        else if (ch == ';')
        {
            t->osc_stage = 1;
            t->osc_len = 0;
            t->osc_item = 0;
        }
        else if (!t->osc_len && ch == 'P')
        {
            // Linux: ESC ] P nrrggbb sets palette entry n, with no terminator.
            t->ansi_state = ansi_state_osc_palette;
            t->osc_len = 0;
        }
        else if (!t->osc_len && ch == 'R')
        {
            // Linux: ESC ] R resets palette 0-15, with no terminator.
            for (unsigned i = 0; i < 16; i++)
                cells_color_reset(t, i);
            t->ansi_state = ansi_state_ground;
        }
        else
            t->osc_stage = 2;
        break;
    case 1: // the items
        if (ch == ';')
        {
            term_osc_item(t);
            t->osc_len = 0;
            if (t->osc_item < 0xFF)
                t->osc_item++;
        }
        else if (t->osc_len < TERM_OSC_SPEC_MAX)
            t->osc_spec[t->osc_len++] = (char)ch;
        else
            t->osc_len = TERM_OSC_SPEC_MAX + 1;
        break;
    }
}

// Linux's ESC ] P nrrggbb: the palette entry n (0-f) and its color, seven
// hex digits and no terminator. Anything else ends it, and is dropped.
static void term_osc_palette_byte(term_t *t, uint8_t ch)
{
    if (term_hex((char)ch) < 0)
    {
        t->ansi_state = ansi_state_ground;
        return;
    }
    t->osc_spec[t->osc_len++] = (char)ch;
    if (t->osc_len < 7)
        return;
    t->ansi_state = ansi_state_ground;
    t->rep_valid = false;
    const char *s = t->osc_spec;
    cells_color(t, (unsigned)term_hex(s[0]),
                     SCANVIDEO_ALPHA_MASK |
                         SCANVIDEO_PIXEL_FROM_RGB8(term_hex_run(&s[1], 2),
                                                   term_hex_run(&s[3], 2),
                                                   term_hex_run(&s[5], 2)));
}

/* ---- the parser -------------------------------------------------------- */

// The C0 controls that act: from the ground state, and from inside a
// sequence for the ones in TERM_C0_IN_SEQ. NUL and BEL do nothing here; the
// console's bell is the RIA's.
static void term_execute(term_t *t, uint8_t ch)
{
    t->rep_valid = false;
    switch (ch)
    {
    case '\b': // BS
    case '\t': // HT
    case '\n': // LF
    case '\v': // VT
    case '\f': // FF: all three are LF, as on a VT102 and in xterm and Linux
    case '\r': // CR
        term_grids(t, GRID_C0, ch);
        break;
    case 0x0E: // SO (Shift Out): activate G1
        term_pen(t)->gl_is_g1 = true;
        break;
    case 0x0F: // SI (Shift In): activate G0
        term_pen(t)->gl_is_g1 = false;
        break;
    }
}

// A graphic character from the stream, which REP can repeat.
static void term_print(term_t *t, uint8_t byte)
{
    t->last_glyph = byte;
    t->rep_valid = true;
    term_grids(t, GRID_PRINT, byte);
}

// A byte in the ground state. Bytes 0x20-0x7E and 0x80-0xFF are glyphs. A C0
// control Linux has no action for is drawn too, and with SGR 11 (or CRM)
// every one but NUL, BS, LF, FF, CR, SO, SI and ESC is, DEL included. ENQ is
// drawn: there is no answerback.
static void term_ground(term_t *t, uint8_t ch)
{
    if (ch >= 0x20 && ch != TERM_DEL)
        term_print(t, ch);
    else if (ch == TERM_DEL)
    {
        if (t->disp_ctrl)
            term_print(t, ch);
    }
    else if (t->disp_ctrl ? !(TERM_C0_ALWAYS & (1u << ch))
                          : !(TERM_C0_ACTION & (1u << ch)))
        term_print(t, ch);
    else
        term_execute(t, ch);
}

// A control or a byte past 0x7E inside an escape sequence. The controls of
// TERM_C0_IN_SEQ act and the sequence goes on; NUL and DEL are ignored; any
// other ends the sequence, silently, as in Linux. Returns false for an
// ordinary sequence byte. CAN, SUB and ESC never get here.
static bool term_seq_control(term_t *t, uint8_t ch)
{
    if (ch >= 0x20 && ch < TERM_DEL)
        return false;
    if (ch && ch != TERM_DEL)
    {
        if (ch < 0x20 && (TERM_C0_IN_SEQ & (1u << ch)))
            term_execute(t, ch);
        else
            t->ansi_state = ansi_state_ground;
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

// The final byte of ESC I... F, a sequence with intermediates: the character
// set designations, and ESC # for the line sizes and DECALN. ESC % (UTF-8),
// ESC SP (7/8-bit controls), G2 and G3 are taken and ignored.
static void term_esc_int_final(term_t *t, uint8_t ch)
{
    t->ansi_state = ansi_state_ground;
    t->rep_valid = false;
    if (t->csi_intermediate == '(')
        term_designate(&term_pen(t)->g0_charset, ch);
    else if (t->csi_intermediate == ')')
        term_designate(&term_pen(t)->g1_charset, ch);
    else if (t->csi_intermediate == '#' && ch >= '3' && ch <= '8' && ch != '7')
        term_grids(t, GRID_LINE, ch);
}

static void term_esc_int_byte(term_t *t, uint8_t ch)
{
    if (term_seq_control(t, ch))
        return;
    if (ch < 0x30)
        t->csi_intermediate = 0xFF; // a second intermediate: nothing we know
    else
        term_esc_int_final(t, ch);
}

static void term_start_csi(term_t *t)
{
    t->ansi_state = ansi_state_csi;
    t->csi_param_count = 0;
    t->csi_param[0] = 0;
    t->csi_separator[0] = 0;
    t->csi_private = 0;
    t->csi_intermediate = 0;
    t->csi_colon = false;
    t->csi_any = false;
}

static void term_start_osc(term_t *t)
{
    t->ansi_state = ansi_state_osc;
    t->osc_ps = 0;
    t->osc_stage = 0;
    t->osc_len = 0;
    t->osc_item = 0;
}

static void term_esc_byte(term_t *t, uint8_t ch)
{
    bool st = t->osc_pending;
    t->osc_pending = false;
    if (term_seq_control(t, ch))
        return;
    if (ch < 0x30)
    {
        t->ansi_state = ansi_state_esc_int;
        t->csi_intermediate = ch;
        return;
    }
    t->ansi_state = ansi_state_ground;
    if (ch != '[' && ch != ']')
        t->rep_valid = false;
    switch (ch)
    {
    case '[': // CSI
        term_start_csi(t);
        break;
    case ']': // OSC
        term_start_osc(t);
        break;
    case 'P': // DCS
    case 'X': // SOS
    case '^': // PM
    case '_': // APC
        t->ansi_state = ansi_state_string;
        break;
    case '\\': // ST: the end of a string, which ends an OSC
        if (st)
            term_osc_end(t);
        break;
    case 'c': // RIS
        term_RIS(t);
        break;
    case 'D': // IND - Index (move down, scroll on margin_bot)
    case 'E': // NEL - Next Line (CR + IND)
    case 'H': // HTS - Horizontal Tab Set at current column
    case 'M': // RI
        term_grids(t, GRID_ESC, ch);
        break;
    case '7': // DECSC
        term_save_cursor(t);
        break;
    case '8': // DECRC
        term_restore_cursor(t);
        break;
    case 'Z': // DECID: answered as Primary DA
        TERM_REPLY(t, "\33[?6c");
        break;
        // SS2 and SS3 (N, O) have no G2 or G3 to shift to, so the next byte
        // prints as itself. The keypad modes (=, >) are not done yet.
    }
}

// The CSI sequences with no private marker and no intermediate. The ones
// that move the cursor or the rows go to every grid.
static void term_csi_dispatch(term_t *t, uint8_t ch)
{
    switch (ch)
    {
    case 'b': // REP: the last glyph again, if nothing came between
        if (t->rep_valid)
            term_grids(t, GRID_REP, 0);
        break;
    case 'c': // Primary DA: a VT102
        if (!t->csi_param[0])
            TERM_REPLY(t, "\33[?6c");
        break;
    case 'h':
        term_out_SM_RM(t, true);
        break;
    case 'l':
        term_out_SM_RM(t, false);
        break;
    case 'm':
        term_out_SGR(t);
        break;
    case 'n':
        term_out_DSR(t);
        break;
    case 's': // DECSC (xterm's SCOSC, the same without left and right margins)
        term_save_cursor(t);
        break;
    case 't':
        term_out_window(t);
        break;
    case 'u': // DECRC
        term_restore_cursor(t);
        break;
    case 'x':
        term_out_DECREQTPARM(t);
        break;
    case 'y': // DECTST: the power-up test is a reset; the loopbacks are ignored
        if (t->csi_param[0] == 2 && t->csi_param_count == 2 &&
            t->csi_param[1] == 1)
            term_RIS(t);
        break;
    default:
        term_grids(t, GRID_CSI, ch);
        break;
    }
    t->rep_valid = false;
}

static void term_csi_final(term_t *t, uint8_t ch)
{
    t->ansi_state = ansi_state_ground;
    if (t->csi_param_count < TERM_CSI_PARAM_MAX_LEN)
        t->csi_param_count++;
    if (t->csi_colon && ch != 'm')
        return; // ':' belongs to SGR alone
    uint8_t inter = t->csi_intermediate;
    switch (t->csi_private)
    {
    case 0:
        if (!inter)
            return term_csi_dispatch(t, ch);
        if (inter == ' ' && ch == 'q')
            term_out_DECSCUSR(t);
        else if (inter == ' ' && (ch == '@' || ch == 'A'))
            term_grids(t, ch == '@' ? GRID_SL : GRID_SR, 0);
        else if (inter == '!' && ch == 'p')
            term_out_DECSTR(t);
        else if (inter == '$' && ch == 'p')
            term_out_DECRQM(t, false);
        break;
    case '?':
        if (!inter && (ch == 'h' || ch == 'l'))
            term_out_DECSET(t, ch == 'h');
        else if (inter == '$' && ch == 'p')
            term_out_DECRQM(t, true);
        break;
    case '>': // Secondary DA: type 0, firmware 1, ROM 0
        if (!inter && ch == 'c' && !t->csi_param[0])
            TERM_REPLY(t, "\33[>0;1;0c");
        break;
    case '=': // Tertiary DA: a unit ID of zeros
        if (!inter && ch == 'c' && !t->csi_param[0])
            TERM_REPLY(t, "\33P!|00000000\33\\");
        break;
    }
    t->rep_valid = false;
}

// A byte of a CSI sequence, after the introducer. Parameters are digits, ';'
// and ':' (0x30-0x3B); a private marker (0x3C-0x3F) counts only as the first
// byte; intermediates (0x20-0x2F) come after the parameters; the final byte
// (0x40-0x7E) runs it. Anything out of that order makes the sequence one to
// drop, through its final byte.
static void term_csi_byte(term_t *t, uint8_t ch)
{
    if (term_seq_control(t, ch))
        return;
    if (ch >= 0x40)
    {
        if (t->ansi_state == ansi_state_csi_ignore || t->csi_intermediate == 0xFF)
        {
            // Malformed, or doubled intermediates: nothing we know.
            t->ansi_state = ansi_state_ground;
            t->rep_valid = false;
        }
        else
            term_csi_final(t, ch);
        return;
    }
    if (t->ansi_state == ansi_state_csi_ignore)
        return;
    if (ch < 0x30)
    {
        t->csi_intermediate = t->csi_intermediate ? 0xFF : ch;
        return;
    }
    if (t->csi_intermediate)
    {
        t->ansi_state = ansi_state_csi_ignore; // a parameter after an intermediate
        return;
    }
    uint8_t i = t->csi_param_count;
    if (ch >= 0x3C)
    {
        if (t->csi_any)
            t->ansi_state = ansi_state_csi_ignore;
        else
            t->csi_private = ch;
        t->csi_any = true;
        return;
    }
    t->csi_any = true;
    if (ch == ';' || ch == ':')
    {
        if (ch == ':')
            t->csi_colon = true;
        if (i < TERM_CSI_PARAM_MAX_LEN)
        {
            t->csi_separator[i] = (char)ch;
            t->csi_param_count = ++i;
            if (i < TERM_CSI_PARAM_MAX_LEN)
            {
                t->csi_param[i] = 0;
                t->csi_separator[i] = 0;
            }
        }
        return;
    }
    if (i < TERM_CSI_PARAM_MAX_LEN)
    {
        uint32_t v = t->csi_param[i] * 10u + (ch - '0');
        t->csi_param[i] = v > 0xFFFF ? 0xFFFF : (uint16_t)v;
    }
}

/* ---- VT52 mode --------------------------------------------------------- */

// ESC and a letter in VT52 mode, as a VT102 takes them there.
static void term_vt52_byte(term_t *t, uint8_t ch)
{
    if (term_seq_control(t, ch))
        return;
    t->ansi_state = ansi_state_ground;
    t->rep_valid = false;
    switch (ch)
    {
    case 'A': // cursor up
    case 'B': // cursor down
    case 'C': // cursor right
    case 'D': // cursor left
    case 'H': // home
    case 'I': // reverse line feed
    case 'J': // erase to end of screen
    case 'K': // erase to end of line
        term_grids(t, GRID_VT52, ch);
        break;
    case 'F': // graphics mode: DEC Special Graphics, as the VT102 and xterm do
        term_pen(t)->g0_charset = TERM_CS_DEC;
        term_pen(t)->gl_is_g1 = false;
        break;
    case 'G': // ASCII mode
        term_pen(t)->g0_charset = TERM_CS_ASCII;
        term_pen(t)->gl_is_g1 = false;
        break;
    case 'Y': // direct cursor address: row and column, each plus 32
        t->ansi_state = ansi_state_vt52_row;
        break;
    case 'Z': // identify: a VT100 in VT52 mode
        TERM_REPLY(t, "\33/Z");
        break;
    case '<': // ANSI mode
        t->vt52 = false;
        break;
        // The keypad modes (=, >) are not done yet.
    }
}

// The row and column of ESC Y. A row off the screen leaves the cursor's row
// alone; a column off it goes to the last column.
static void term_vt52_address(term_t *t, uint8_t ch)
{
    if (term_seq_control(t, ch))
        return;
    if (t->ansi_state == ansi_state_vt52_row)
    {
        t->csi_param[0] = ch;
        t->ansi_state = ansi_state_vt52_col;
        return;
    }
    t->ansi_state = ansi_state_ground;
    t->rep_valid = false;
    term_grids(t, GRID_VT52_Y, ch);
}

void term_write(term_t *t, uint8_t ch)
{
    ansi_state_t state = t->ansi_state;
    if (ch == TERM_C0_ESC)
    {
        // ESC starts a new sequence from any state; in an OSC it may be ST.
        t->osc_pending = (state == ansi_state_osc);
        t->ansi_state = t->vt52 ? ansi_state_vt52_esc : ansi_state_esc;
        return;
    }
    if (state != ansi_state_ground && (ch == TERM_C0_CAN || ch == TERM_C0_SUB))
    {
        // CAN and SUB abort a sequence or a string, silently. An OSC's items
        // apply as they arrive, so the colors keep what an aborted one set.
        t->ansi_state = ansi_state_ground;
        t->rep_valid = false;
        return;
    }
    switch (state)
    {
    case ansi_state_ground:
        term_ground(t, ch);
        break;
    case ansi_state_esc:
        term_esc_byte(t, ch);
        break;
    case ansi_state_esc_int:
        term_esc_int_byte(t, ch);
        break;
    case ansi_state_csi:
    case ansi_state_csi_ignore:
        term_csi_byte(t, ch);
        break;
    case ansi_state_osc:
        term_osc_byte(t, ch);
        break;
    case ansi_state_osc_palette:
        term_osc_palette_byte(t, ch);
        break;
    case ansi_state_string:
        break;
    case ansi_state_vt52_esc:
        term_vt52_byte(t, ch);
        break;
    case ansi_state_vt52_row:
    case ansi_state_vt52_col:
        term_vt52_address(t, ch);
        break;
    case ansi_state_count:
        break;
    }
}

/* ---- savestate ----------------------------------------------------------
 *
 * Written field by field. term_pen_t has padding, and a struct copy would put
 * whatever is in it into the blob, where it would make two saves of one
 * unchanged machine differ. The grids are saved by their owner, with
 * term_grid_sst_put. */

static void term_put_color(sst_cursor_t *c, const term_color_t *col)
{
    sst_put_u8(c, col->kind);
    sst_put_u8(c, col->index);
    sst_put_u16(c, col->rgb);
}

static void term_get_color(sst_cursor_t *c, term_color_t *col)
{
    col->kind = sst_get_u8(c);
    col->index = sst_get_u8(c);
    col->rgb = sst_get_u16(c);
}

static void term_put_pen(sst_cursor_t *c, const term_pen_t *pen)
{
    sst_put_u8(c, pen->sgr_attr);
    term_put_color(c, &pen->fg);
    term_put_color(c, &pen->bg);
    term_put_color(c, &pen->ul);
    sst_put_bool(c, pen->ul_set);
    sst_put_bool(c, pen->bold);
    sst_put_bool(c, pen->faint);
    sst_put_bool(c, pen->reverse);
    sst_put_bool(c, pen->conceal);
    sst_put_bool(c, pen->origin_mode);
    sst_put_u8(c, pen->g0_charset);
    sst_put_u8(c, pen->g1_charset);
    sst_put_bool(c, pen->gl_is_g1);
}

static void term_get_pen(sst_cursor_t *c, term_pen_t *pen)
{
    pen->sgr_attr = sst_get_u8(c);
    term_get_color(c, &pen->fg);
    term_get_color(c, &pen->bg);
    term_get_color(c, &pen->ul);
    pen->ul_set = sst_get_bool(c);
    pen->bold = sst_get_bool(c);
    pen->faint = sst_get_bool(c);
    pen->reverse = sst_get_bool(c);
    pen->conceal = sst_get_bool(c);
    pen->origin_mode = sst_get_bool(c);
    pen->g0_charset = sst_get_u8(c);
    pen->g1_charset = sst_get_u8(c);
    pen->gl_is_g1 = sst_get_bool(c);
}

void term_sst_put(sst_cursor_t *c, const term_t *t)
{
    for (int s = 0; s < 2; s++)
    {
        term_put_pen(c, &t->pen[s]);
        term_put_pen(c, &t->saved[s]);
        sst_put_bool(c, t->has_saved[s]);
    }
    sst_put_bool(c, t->alt_active);
    sst_put_bool(c, t->autowrap);
    sst_put_bool(c, t->insert_mode);
    sst_put_bool(c, t->newline_mode);
    sst_put_bool(c, t->disp_ctrl);
    sst_put_bool(c, t->vt52);
    sst_put_bool(c, t->ice_colors);
    sst_put_bool(c, t->reverse_screen);
    sst_put_bool(c, t->cursor_enabled);
    sst_put_u8(c, t->cursor_style);
    sst_put_u8(c, t->last_glyph);
    sst_put_bool(c, t->rep_valid);
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
}

/* The grids are the owner's and are left as they are. Everything that
 * indexes is bounded: the parser's counts their arrays, and in the Linux
 * palette sequence osc_len the spec it is filling. The alternate screen only
 * exists in a build that has one. */
bool term_sst_get(sst_cursor_t *c, term_t *t)
{
    for (int s = 0; s < 2; s++)
    {
        term_get_pen(c, &t->pen[s]);
        term_get_pen(c, &t->saved[s]);
        t->has_saved[s] = sst_get_bool(c);
    }
    bool alt = sst_get_bool(c);
    t->autowrap = sst_get_bool(c);
    t->insert_mode = sst_get_bool(c);
    t->newline_mode = sst_get_bool(c);
    t->disp_ctrl = sst_get_bool(c);
    t->vt52 = sst_get_bool(c);
    t->ice_colors = sst_get_bool(c);
    t->reverse_screen = sst_get_bool(c);
    t->cursor_enabled = sst_get_bool(c);
    t->cursor_style = sst_get_u8(c);
    t->last_glyph = sst_get_u8(c);
    t->rep_valid = sst_get_bool(c);
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
    if (!sst_ok(c) || (alt && !TERM_ALT_SCREEN))
        return false;
    if (state >= ansi_state_count ||
        t->csi_param_count > TERM_CSI_PARAM_MAX_LEN || t->osc_len > TERM_OSC_SPEC_MAX + 1 ||
        (state == ansi_state_osc_palette && t->osc_len >= 7))
        return false;
    t->ansi_state = (ansi_state_t)state;
    term_show(t, alt);
    return true;
}
