/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_VGA_MODE0_H_
#define _CORE_VGA_MODE0_H_

/* Mode 0 draws the console's terminal. Everything it draws from is here:
 * the cells and a few words that say which cells make each row, where the
 * cursor is and how the screen looks. cells.c writes them and the renderer
 * reads them: mode0.c on a machine that draws in software, mode0.sv on one
 * that draws in fabric, where the linker places them on its memories. */

#include "core/term/term.h"
#include "core/vga/pixel_format.h"
#include "core/vga/prog.h"
#include "core/vga/vga.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* A cell is in two tables, indexed alike. mode0_cells has its glyph and
 * its attributes in one word, which is all a scanline reads. mode0_colors
 * has its three colors, which are read when a row's pixel pairs are built.
 * cells.c takes both from the pen as the cell is written.
 *
 * The glyph is its place in a row of the font (core/term/font.h), so a
 * scanline draws any cell with one lookup. A character of the code page is
 * its code. Italics and DEC Special Graphics are in the font's second
 * table, at MODE0_GLYPH_OTHER and on, and the size of the glyphs is in
 * that, so the 40- and 80-column cells of one character differ.
 *
 * The attributes are the TERM_ATTR_ bits of TERM_ATTR_CELL_MASK, which the
 * renderer draws, and above them:
 *
 * MODE0_ATTR_UL_COLOR: the underline has a color of its own, SGR 58's.
 * Without it the underline is the foreground's, and a scanline draws it
 * with the glyph and reads no color.
 *
 * MODE0_ATTR_GUARD: DECSCA or SPA guards the cell from the selective
 * erases.
 *
 * MODE0_ATTR_BOLD: bold made the cell's ink the bright entry of its
 * palette color, 8 above it, and with MODE0_ATTR_BOLD_DEFAULT the ink was
 * the default foreground and is entry 15.
 *
 * MODE0_ATTR_REVERSE: the colors are swapped, so the ink is the
 * background.
 *
 * The guard, the bolds and the reverse are the console's own, for the
 * sequences that change the attributes of cells already written (DECCARA
 * and DECRARA). No renderer draws them: the colors already show them.
 *
 * Only the underlines, the strike and the overline change what a scanline
 * draws, and only on their own rows of the glyph. On every other row a
 * scanline reads the glyph alone. */
typedef struct
{
    uint16_t glyph;
    uint16_t attributes;
} mode0_cell_t;
#define MODE0_GLYPH_OTHER(rows) (0x100u * (rows))
#define MODE0_GLYPH_ITALIC(rows, code) ((uint16_t)(MODE0_GLYPH_OTHER(rows) + (code)))
#define MODE0_GLYPH_DEC(rows, code) ((uint16_t)(MODE0_GLYPH_OTHER(rows) + 0x80u + (code)))
#define MODE0_ATTR_UL_COLOR (1u << 8)
#define MODE0_ATTR_GUARD (1u << 9)
#define MODE0_ATTR_BOLD (1u << 10)
#define MODE0_ATTR_BOLD_DEFAULT (1u << 11)
#define MODE0_ATTR_REVERSE (1u << 12)
/* The attributes that put a line through a cell. */
#define MODE0_ATTR_LINES (TERM_ATTR_UL_MASK | TERM_ATTR_STRIKE | TERM_ATTR_OVERLINE)

/* Whether a glyph is in a font of this many rows: the code page's 256, the
 * italics of ASCII, or DEC Special Graphics's 0x5F to 0x7E. */
static inline bool mode0_glyph_ok(uint16_t glyph, unsigned rows)
{
    return glyph < 0x100 || (uint16_t)(glyph - MODE0_GLYPH_OTHER(rows)) < 0x80 ||
           (uint16_t)(glyph - MODE0_GLYPH_DEC(rows, 0x5F)) < 0x20;
}

/* The character a glyph is, and whether it is DEC Special Graphics's. */
static inline uint8_t mode0_glyph_code(uint16_t glyph)
{
    return glyph < 0x100 ? (uint8_t)glyph : (uint8_t)(glyph & 0x7F);
}

static inline bool mode0_glyph_is_dec(uint16_t glyph)
{
    return glyph >= 0x100 && (glyph & 0x80);
}

/* The underlines: the pixels of each TERM_UL_ style, leftmost in bit 7, on
 * the last three rows of a glyph, which are [1] to [3]. Row [0] is every
 * other row of the glyph and holds nothing. One for the 16 rows of the
 * 80-column glyphs and one for the 8 rows of the 40-column ones. A style's
 * pixels are the same in every cell, so a run of cells joins up. */
extern const uint8_t mode0_underline16[4][8];
extern const uint8_t mode0_underline8[4][8];

/* The underline color is SGR 58's, or the foreground's if none is set.
 *
 * A color is one of two things, told apart by the pixel format's opaque
 * bit. Set, it is a pixel, drawn as it is: an RGB color. Clear, it is an
 * entry of mode0_palette, in bits 15:6, so a cell drawn in a palette color
 * or a default color takes a change to that color with no one touching the
 * cell. Whether such a color is transparent is the entry's to say. */
typedef struct
{
    uint16_t fg_color;
    uint16_t bg_color;
    uint16_t ul_color;
} mode0_colors_t;

/* The colors a cell can name: the 256 palette entries and the default
 * foreground and background, then all of those again at half brightness,
 * for SGR 2. cells.c writes them. */
#define MODE0_ENTRIES 258
#define MODE0_DEFAULT_FG 256
#define MODE0_DEFAULT_BG 257
#define MODE0_ENTRY_SHIFT 6
#define MODE0_ENTRY(entry, faint) \
    ((uint16_t)(((unsigned)(entry) + ((faint) ? MODE0_ENTRIES : 0u)) << MODE0_ENTRY_SHIFT))
extern uint16_t mode0_palette[2 * MODE0_ENTRIES];

_Static_assert(MODE0_ENTRY_SHIFT == SCANVIDEO_ALPHA_PIN + 1,
               "an entry sits above the opaque bit, which is clear for one");

/* The pixel a cell's color is drawn as. */
static inline uint16_t mode0_color(uint16_t color)
{
    return (color & SCANVIDEO_ALPHA_MASK) ? color : mode0_palette[color >> MODE0_ENTRY_SHIFT];
}

_Static_assert(sizeof(mode0_cell_t) == 4, "a cell is a word");
_Static_assert(sizeof(mode0_colors_t) == 6, "a cell's colors are three halfwords");

/* The cells of every screen: the 40-column terminal's primary and
 * alternate, then the 80-column terminal's. */
#define MODE0_SCREENS (TERM_ALT_SCREEN ? 2 : 1)
#define MODE0_CELLS ((40 + 80) * TERM_MAX_HEIGHT * MODE0_SCREENS)
extern mode0_cell_t mode0_cells[MODE0_CELLS];
extern mode0_colors_t mode0_colors[MODE0_CELLS];

/* The words, all of them whole 32-bit words, because a fabric's registers
 * take nothing smaller. They describe the terminal showing.
 *
 * row: a row's first cell in mode0_cells, and its TERM_LINE_ size.
 * cursor: x; y; the DECSCUSR style; whether its blink has it lit; and
 *   whether it is shown at all.
 * cursor_color: OSC 12's color.
 * screen: the blink phase a cell's TERM_ATTR_ANY_BLINK bits are tested
 *   against, and DECSCNM. */
#define MODE0_ROWS 32
typedef struct
{
    uint32_t row[MODE0_ROWS];
    uint32_t cursor;
    uint32_t cursor_color;
    uint32_t screen;
} mode0_regs_t;

_Static_assert(TERM_MAX_HEIGHT <= MODE0_ROWS, "a row word for every row");
_Static_assert(MODE0_CELLS <= 0x10000, "a row word holds a 16-bit cell");

extern volatile mode0_regs_t mode0_regs;

#define MODE0_ROW(cell, size) ((uint32_t)(cell) | ((uint32_t)(size) << 16))
#define MODE0_ROW_CELL(w) ((uint16_t)(w))
#define MODE0_ROW_SIZE(w) ((uint8_t)(((w) >> 16) & 3))

#define MODE0_CURSOR_ENABLED (1u << 25)
#define MODE0_CURSOR_LIT (1u << 24)
#define MODE0_CURSOR(x, y, style) \
    ((uint32_t)(x) | ((uint32_t)(y) << 8) | ((uint32_t)(style) << 16))
#define MODE0_CURSOR_X(w) ((uint8_t)(w))
#define MODE0_CURSOR_Y(w) ((uint8_t)((w) >> 8))
#define MODE0_CURSOR_STYLE(w) ((uint8_t)((w) >> 16))

#define MODE0_SCREEN_REVERSE (1u << 2)
#define MODE0_SCREEN_BLINK(w) ((uint8_t)((w) & 3))

bool mode0_prog(uint16_t *xregs);

/* The terminal's place on a canvas w by h, which every machine that programs
 * mode 0 works out the same way. BEGIN and END both 0 give the tallest
 * terminal that fits, centered, so a canvas that is not a whole number of
 * rows tall splits the spare scanlines above and below it: 2..178 at 180 and
 * 4..356 at 360. END 0 alone runs to the bottom of the canvas. False when the
 * scanlines between BEGIN and END are not a whole number of rows. */
static inline bool mode0_layout(int16_t w, int16_t h,
                                int16_t *begin, int16_t *end,
                                uint8_t *cols, uint8_t *rows)
{
    const int16_t px = (int16_t)vga_term_row_px(w);
    if (!*begin && !*end)
    {
        *begin = (int16_t)(h % px / 2);
        *end = (int16_t)(*begin + h / px * px);
    }
    if (!*end)
        *end = h;
    const int16_t count = (int16_t)(*end - *begin);
    if (!count || count % px)
        return false;
    *cols = (uint8_t)vga_term_columns(w);
    *rows = (uint8_t)(count / px);
    return true;
}

/* Whether a mode 0 program of xregs (PLANE, BEGIN and END at 2, 3 and 4)
 * succeeds on a canvas w by h, its scanlines, and the terminal it gives. The
 * VGA programs mode 0 with it, and the RIA works out the VGA's terminal with
 * it as it sends the xreg, without waiting for the ack. */
static inline bool mode0_place(int16_t w, int16_t h, const uint16_t *xregs,
                               int16_t *begin, int16_t *end,
                               uint8_t *cols, uint8_t *rows)
{
    int16_t plane = (int16_t)xregs[2];
    *begin = (int16_t)xregs[3];
    *end = (int16_t)xregs[4];
    if (!mode0_layout(w, h, begin, end, cols, rows))
        return false;
    return plane >= 0 && plane < SCANVIDEO_PLANE_COUNT &&
           *begin >= 0 && *end <= h && *end - *begin >= 1;
}

/* The renderer, for options of 0, which is all mode 0 has. A fabric build
 * has no renderers. */
#ifndef RP6502_VGA_FABRIC
vga_fill_fn_t mode0_fill_fn(uint16_t options);
#endif

#endif /* _CORE_VGA_MODE0_H_ */
