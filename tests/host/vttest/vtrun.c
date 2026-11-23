/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Runs Thomas Dickey's vttest against the console's terminal, term.c, with
 * nobody at the keyboard. vttest runs in a pty. Everything it writes goes to
 * the terminal, and the terminal's replies go back to it, the way they would
 * reach a host. Every time vttest waits for a key the screen is recorded, and
 * the walk answers: the next choice on a menu, 0 on a menu with nothing left,
 * the keys a test asks for, and Return anywhere else. The walk types on the
 * machine's keyboard, so vttest gets what the keys send in the modes the
 * terminal is in. It runs on Linux, which shows when vttest is blocked
 * reading its terminal.
 *
 * usage: vtrun OUTDIR [vttest arguments]
 *
 * A menu goes by the number in its title, as 11.1 for "Menu 11.1: ...", or
 * main, or m03 for the fourth menu met where it has no title. A screen is
 * named after the menu choice that led to it and its place after that
 * choice: 11.1_3-04 is the fourth screen of choice 3 of menu 11.1, and
 * 11.1-menu-02 is that menu the second time it looked different. A screen
 * that a test clears without waiting for a key is recorded too, just before
 * the clear.
 *
 * OUTDIR/screens.txt has a line a screen: its name, a CRC-32 of what it
 * shows (the cells' glyphs, attributes and colors, the line sizes and the
 * reverse screen), and the cursor. OUTDIR/index.txt is the
 * walk, with the keys typed after each screen. With VTGOLDEN naming an
 * approved screens.txt, the run fails when a screen differs, is new, or is
 * gone, and only those screens are saved as text and PNG for a person to look
 * at; without it, every screen is.
 *
 * With VTXTERM set, to an xterm's path or to 1 for the one on the PATH, a
 * real xterm is given the same bytes, and every screen
 * recorded is held against xterm's (xmirror.h). The differences are in
 * OUTDIR/xterm.txt and OUTDIR/xterm/, and VTXKNOWN names those approved.
 *
 * In the mouse's menus (11.8.5) the walk takes the choices in an order of its
 * own: every test of the mouse's events in every form of report, with focus
 * events on, then the DEC locator's. A test there gets the mouse as well as
 * keys: the pointer moves over the console's cells, its buttons go down and
 * up and its wheel turns, through the tablet as a host's pointer does, and
 * the focus goes and comes back. A choice taken more than once names its
 * screens with the time it was taken, as 11.8.5_3.2-01. The selection is in
 * a screen's digest while one shows.
 *
 * With VTCMD set, the program run is that shell command instead, with TERM
 * xterm-256color, and the walk is VTKEYS: a screen is recorded each time
 * the program has been quiet a moment, then the next step of VTKEYS is
 * typed, steps between |, words as a test's answer takes them. */

#include "core/com/com.h"
#include "core/com/manifold.h"
#include "core/hid/hostkey.h"
#include "core/hid/keyboard.h"
#include "core/hid/tablet.h"
#include "core/hid/usage.h"
#include "core/hid/vtkey.h"
#include "core/sys/sys.h"
#include "core/term/draw.h"
#include "core/term/vterm.h"
#include "core/term/vterm_mouse.h"
#include "tb_screen.h"
#include "xmirror.h"
#include "core/vga/vga.h"
#include "core/vga/vga_emu.h"
#include "host/host.h"
#include "host/sokol/cli/png.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define VT_POLL_MS 2      /* how often the walk looks for vttest waiting */
#define VT_STUCK_MS 7000  /* quiet and not reading this long, and it waits anyway:
                           * longer than its longest pause, the 5 s after a RIS */
#define VT_STEP_LIMIT 4000 /* screens before giving up on a walk gone wrong */
#define VT_TEST_LIMIT 60   /* screens one test may take before it is left */
#define VT_TIME_LIMIT 480  /* seconds for the whole walk, under ctest's 600 */

static int vt_master = -1;

/* What vttest has read in all, from /proc: the bytes its reads returned. */
static pid_t vt_pid;
static uint64_t vt_read_count(void)
{
    char path[64], line[128];
    snprintf(path, sizeof path, "/proc/%d/io", (int)vt_pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    unsigned long long n = 0;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "rchar: %llu", &n) == 1)
            break;
    fclose(f);
    return n;
}

/* What was written to vttest and it has yet to read: vt_owed bytes, counted
 * from where its reads stood at vt_owed_from. A write to a pseudoterminal
 * reaches the other side later, from a worker of the kernel's, and until it
 * does vttest still sleeps in its read with nothing to read, which is how it
 * looks when it waits for a key. So it is not waiting until it has read
 * what it was sent. */
static uint64_t vt_owed, vt_owed_from;

static bool vt_taken(void)
{
    if (vt_owed && vt_read_count() - vt_owed_from >= vt_owed)
        vt_owed = 0;
    return !vt_owed;
}

static void vt_reply(const char *s, size_t n)
{
    if (!vt_taken() || n)
    {
        if (!vt_owed)
            vt_owed_from = vt_read_count();
        vt_owed += n;
    }
    while (n)
    {
        ssize_t w = write(vt_master, s, n);
        if (w <= 0)
            return;
        s += w;
        n -= (size_t)w;
    }
}

/* Writes code point cp as UTF-8 at out, returns the bytes written. */
static size_t vt_utf8(uint32_t cp, char *out)
{
    if (cp < 0x80)
    {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800)
    {
        out[0] = (char)(0xC0 | cp >> 6);
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    out[0] = (char)(0xE0 | cp >> 12);
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}

/* ------------------------------------------------------------------------
 * The terminal */

static uint32_t vt_fb[VGA_MAX_WIDTH * VGA_MAX_HEIGHT];
static int vt_rows, vt_cols;
static bool vt_mirror; /* xterm is given what the terminal is */
static FILE *vt_stream; /* and OUTDIR/stream.bin has all of it */
static long vt_streamed;

/* The DEC special graphics, 0x5F to 0x7E, as Unicode. */
static const uint32_t vt_dec[32] = {
    ' ', 0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0,
    0x00B1, 0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C,
    0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534,
    0x252C, 0x2502, 0x2A7D, 0x2A7E, 0x03C0, 0x2260, 0x00A3, 0x00B7,
};

/* DEC Technical, 0x21 to 0x7E, as Unicode, as xterm has it. The pieces of
 * the big sigma have no Unicode and are xterm's private ones; the codes
 * DEC left undefined are spaces. */
static const uint32_t vt_tech[0x5E] = {
    0x23B7, 0x250C, 0x2500, 0x2320, 0x2321, 0x2502, 0x23A1, 0x23A3,
    0x23A4, 0x23A6, 0x23A7, 0x23A9, 0x23AB, 0x23AD, 0x23A8, 0x23AC,
    0xE000, 0xE001, 0xE002, 0xE003, 0xE004, 0xE005, 0xE006, ' ',
    ' ', ' ', ' ', 0x2264, 0x2260, 0x2265, 0x222B, 0x2234,
    0x221D, 0x221E, 0x00F7, 0x0394, 0x2207, 0x03A6, 0x0393, 0x223C,
    0x2243, 0x0398, 0x00D7, 0x039B, 0x21D4, 0x21D2, 0x2261, 0x03A0,
    0x03A8, ' ', 0x03A3, ' ', ' ', 0x221A, 0x03A9, 0x039E,
    0x03A5, 0x2282, 0x2283, 0x2229, 0x222A, 0x2227, 0x2228, 0x00AC,
    0x03B1, 0x03B2, 0x03C7, 0x03B4, 0x03B5, 0x03C6, 0x03B3, 0x03B7,
    0x03B9, 0x03B8, 0x03BA, 0x03BB, ' ', 0x03BD, 0x2202, 0x03C0,
    0x03C8, 0x03C1, 0x03C3, 0x03C4, ' ', 0x0192, 0x03C9, 0x03BE,
    0x03C5, 0x03B6, 0x2190, 0x2191, 0x2192, 0x2193,
};

/* The CP437 glyphs of the C0 codes, which term.c draws for the controls it
 * has no action for. */
static const uint32_t vt_cp437_low[32] = {
    ' ', 0x263A, 0x263B, 0x2665, 0x2666, 0x2663, 0x2660, 0x2022,
    0x25D8, 0x25CB, 0x25D9, 0x2642, 0x2640, 0x266A, 0x266B, 0x263C,
    0x25BA, 0x25C4, 0x2195, 0x203C, 0x00B6, 0x00A7, 0x25AC, 0x21A8,
    0x2191, 0x2193, 0x2192, 0x2190, 0x221F, 0x2194, 0x25B2, 0x25BC,
};

/* The CP437 glyphs of 0x80 to 0xFF, the code page the console starts in. */
static const uint32_t vt_cp437_high[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
    0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

/* What the console's mouse types goes to vttest as it would from the
 * console: its reports, and the DEC locator's answers. */
static void vt_mouse_sends(void)
{
    vterm_mouse_task();
    char buf[64];
    size_t n;
    while ((n = com_read(COM_SOURCE_KEYBOARD, buf, sizeof buf)) > 0)
        vt_reply(buf, n);
}

/* The bytes go to the terminal as a program's would: a byte held for a smooth
 * scroll waits in the console's output while frames run, as many as the
 * scroll takes. All of them are in before the answers are read, because
 * vttest may be waiting for one. The mouse's answers, which it types with
 * the keys, go too. */
static void vt_term_write(const char *buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        while (!manifold_cts())
            vga_run_frame();
        manifold_write_to(COM_TERM_SCREEN, buf + i, 1);
    }
    if (vt_mirror)
    {
        xm_write(buf, n);
        fwrite(buf, 1, n, vt_stream);
        vt_streamed += n;
    }
    while (!manifold_drained())
        vga_run_frame();
    char reply[256];
    size_t r;
    while ((r = com_read(COM_SOURCE_UART, reply, sizeof reply)) > 0)
        vt_reply(reply, r);
    vt_mouse_sends();
}

static void vt_term_init(void)
{
    /* No framebuffer: the screens are judged by their cells, and the frames
     * the walk runs go by without drawing. vt_save draws the one it keeps. */
    sys_init();
    for (int i = 0; i < 120; i++)
        vga_run_frame();
    /* The monitor's banner goes, and the run starts on a blank screen. */
    vt_term_write("\30\33c", 3);
    tb_screen_t v;
    tb_screen(&v);
    vt_rows = v.height;
    vt_cols = v.width;
}

/* What a glyph shows, as Unicode. */
static uint32_t vt_unicode(uint16_t glyph)
{
    uint32_t cp = mode0_glyph_code(glyph);
    if (mode0_glyph_is_tech(glyph))
        return vt_tech[cp - 0x21];
    if (mode0_glyph_is_dec(glyph) && cp >= 0x5F && cp <= 0x7E)
        return vt_dec[cp - 0x5F];
    if (glyph >= 0x100)
        return cp; /* an italic */
    if (cp < 0x20)
        return vt_cp437_low[cp]; /* a control drawn as its glyph */
    if (cp == 0x7F)
        return 0x2302; /* DEL drawn: the house */
    if (cp > 0x7F)
        return vt_cp437_high[cp - 0x80];
    return cp;
}

static void vt_row(int y, char *out, size_t size)
{
    const mode0_cell_t *row = tb_screen_row((uint8_t)y);
    size_t o = 0, last = 0;
    for (int x = 0; x < vt_cols && o + 8 < size; x++)
    {
        uint32_t cp = vt_unicode(row[x].glyph);
        o += vt_utf8(cp, out + o);
        if (cp != ' ')
            last = o;
    }
    out[last] = 0;
}

/* Publishes the screen, and shows blinking text in its lit phase. */
static void vt_settle(void)
{
    draw_task();
    vga_run_frame();
    tb_screen_t v;
    for (int i = 0; i < 120; i++)
    {
        tb_screen(&v);
        if (!v.blink_phase)
            break;
        vga_run_frame();
    }
}

/* A CRC-32 of the cells: glyphs, attributes and the colors they show, with
 * their bold, conceal and reverse drawn (mode0_cell_shows), and the
 * underline's color the ink's but for one of its own. A cell's color is a
 * pixel or an entry of mode 0's palette; the pixel it is drawn as is what
 * goes in, so the golden screens hold whichever a cell has. */
/* A cell's attributes as the byte golden.txt's digests were made from:
 * the blinks in bits 0 and 1, an underline that is not double in bit 2 and
 * a double one in bit 3, then the strike, the overline, DEC graphics and
 * italics. */
static uint8_t vt_attributes(const mode0_cell_t *c)
{
    const uint16_t a = c->attributes;
    const unsigned ul = (a & TERM_ATTR_UL_MASK) >> TERM_ATTR_UL_SHIFT;
    return (uint8_t)((a & TERM_ATTR_ANY_BLINK) |
                     (ul == TERM_UL_DOUBLE ? 8 : ul ? 4 : 0) |
                     (a & TERM_ATTR_STRIKE ? 16 : 0) | (a & TERM_ATTR_OVERLINE ? 32 : 0) |
                     (mode0_glyph_is_dec(c->glyph) ? 64 : 0) |
                     (c->glyph >= 0x100 && !mode0_glyph_is_dec(c->glyph) ? 128 : 0));
}

static uint32_t vt_cells(void)
{
    uint32_t crc = 0;
    for (int y = 0; y < vt_rows; y++)
    {
        const mode0_cell_t *row = tb_screen_row((uint8_t)y);
        const mode0_colors_t *colors = tb_screen_row_colors((uint8_t)y);
        for (int x = 0; x < vt_cols; x++)
        {
            uint16_t fg, bg;
            mode0_cell_shows(&row[x], &colors[x], &fg, &bg);
            const uint16_t ul =
                (row[x].attributes & MODE0_ATTR_UL_COLOR) ? colors[x].ul_color : fg;
            /* The eight bytes golden.txt's digests were made from. */
            const struct
            {
                uint8_t font_code;
                uint8_t attributes;
                uint16_t fg_color;
                uint16_t bg_color;
                uint16_t ul_color;
            } shown = {mode0_glyph_code(row[x].glyph), vt_attributes(&row[x]),
                       mode0_color(fg), mode0_color(bg), mode0_color(ul)};
            crc = host_crc32(crc, &shown, sizeof shown);
        }
    }
    return crc;
}

/* Where the selection starts and ends on the screen showing, rows and
 * columns from 0: false when none shows there. */
static bool vt_selection(uint8_t at[4])
{
    const screen_selection_t *sel = &vterm_selection;
    const term_grid_t *g = vterm_get_shown(&vterm);
    if (!sel->shows || sel->screen != g->shown)
        return false;
    bool start = false, end = false;
    for (uint8_t y = 0; y < g->height; y++)
    {
        const uint8_t m = screen_mem(g->shown, y);
        if (m == sel->start_row)
        {
            at[0] = y;
            at[1] = sel->start_col;
            start = true;
        }
        if (m == sel->end_row)
        {
            at[2] = y;
            at[3] = sel->end_col;
            end = true;
        }
    }
    return start && end;
}

/* A CRC-32 of what the screen shows: the cells, each line's size, and the
 * reverse screen; and the selection, while one shows. */
static uint32_t vt_digest(void)
{
    uint32_t crc = vt_cells();
    for (int y = 0; y < vt_rows; y++)
    {
        uint8_t size = tb_screen_row_size((uint8_t)y);
        crc = host_crc32(crc, &size, 1);
    }
    tb_screen_t v;
    tb_screen(&v);
    uint8_t reverse = v.reverse;
    crc = host_crc32(crc, &reverse, 1);
    uint8_t at[4];
    if (vt_selection(at))
        crc = host_crc32(crc, at, sizeof at);
    return crc;
}

static bool vt_blank(void)
{
    char line[1024];
    for (int y = 0; y < vt_rows; y++)
    {
        vt_row(y, line, sizeof line);
        if (line[0])
            return false;
    }
    return true;
}

static bool vt_screen_has(const char *needle)
{
    char line[1024];
    for (int y = 0; y < vt_rows; y++)
    {
        vt_row(y, line, sizeof line);
        if (strstr(line, needle))
            return true;
    }
    return false;
}

/* ------------------------------------------------------------------------
 * The record */

static const char *vt_outdir;
static FILE *vt_index;
static FILE *vt_screens;
static char vt_path[64] = "start"; /* the choice that led here */
static unsigned vt_seq;              /* screens since that choice */
static uint32_t vt_last;             /* the cells of the last screen recorded */
static size_t vt_fed;                /* bytes from vttest since then */

/* The approved screens, from VTGOLDEN. */
typedef struct
{
    char line[160];
    bool seen;
} vt_golden_t;
#define VT_GOLDEN_MAX 1024
static vt_golden_t vt_golden[VT_GOLDEN_MAX];
static int vt_golden_count;
static bool vt_have_golden;
static int vt_differ;

/* A line of the index, and the same on stdout, so a run that fails in CI
 * shows how far it got. */
static void vt_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(vt_index, fmt, ap);
    va_end(ap);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(vt_index);
    fflush(stdout);
}

static void vt_load_golden(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
    {
        fprintf(stderr, "vtrun: cannot read %s\n", path);
        exit(2);
    }
    char line[160];
    while (fgets(line, sizeof line, f) && vt_golden_count < VT_GOLDEN_MAX)
    {
        line[strcspn(line, "\n")] = 0;
        if (line[0])
            snprintf(vt_golden[vt_golden_count++].line, sizeof vt_golden[0].line, "%s", line);
    }
    fclose(f);
    vt_have_golden = true;
}

/* Whether the screen's line matches the golden line of the same name. */
static bool vt_approved(const char *name, const char *line)
{
    size_t n = strlen(name);
    for (int i = 0; i < vt_golden_count; i++)
    {
        vt_golden_t *g = &vt_golden[i];
        if (!strncmp(g->line, name, n) && g->line[n] == ' ')
        {
            g->seen = true;
            if (!strcmp(g->line, line))
                return true;
            fprintf(stderr, "vtrun: %s\n  want %s\n", line, g->line);
            return false;
        }
    }
    fprintf(stderr, "vtrun: %s is new\n", line);
    return false;
}

/* ------------------------------------------------------------------------
 * xterm, given the same bytes, when VTXTERM is set: each screen recorded is
 * held against xterm's, cell by cell, as the colors and attributes a cell
 * shows rather than how either keeps them. A screen that differs is named in
 * OUTDIR/xterm.txt with a digest of its differences, and OUTDIR/xterm/NAME.txt
 * shows both. VTXKNOWN names the differences approved, in the same form: a
 * difference not there, or not the same, fails the run, as does one there
 * that no longer happens. */

static FILE *vt_xterm;
static int vt_xdiffer;

#define VT_KNOWN_MAX 1024
static vt_golden_t vt_known[VT_KNOWN_MAX];
static int vt_known_count;

static void vt_load_known(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
    {
        fprintf(stderr, "vtrun: cannot read %s\n", path);
        exit(2);
    }
    char line[160];
    while (fgets(line, sizeof line, f) && vt_known_count < VT_KNOWN_MAX)
    {
        line[strcspn(line, "\n")] = 0;
        if (line[0] && line[0] != '#')
            snprintf(vt_known[vt_known_count++].line, sizeof vt_known[0].line, "%s", line);
    }
    fclose(f);
}

/* Whether name with this digest is an approved difference. Its line is the
 * name, the digest, and why. */
static bool vt_known_difference(const char *name, uint32_t digest)
{
    char want[48];
    snprintf(want, sizeof want, "%s %08X", name, digest);
    size_t n = strlen(want);
    for (int i = 0; i < vt_known_count; i++)
        if (!strncmp(vt_known[i].line, want, n) &&
            (vt_known[i].line[n] == ' ' || !vt_known[i].line[n]))
        {
            vt_known[i].seen = true;
            return true;
        }
    return false;
}

/* The console's screen as xm_screen_t has a screen. */
static void vt_ours(xm_screen_t *o)
{
    tb_screen_t v;
    tb_screen(&v);
    static const uint8_t esc_hash[4] = {5, 6, 3, 4}; /* by TERM_LINE_ */
    o->rows = vt_rows;
    o->cols = vt_cols;
    o->cursor_x = v.cursor_x;
    o->cursor_y = v.cursor_y;
    o->reverse = v.reverse;
    for (int y = 0; y < vt_rows; y++)
    {
        const mode0_cell_t *row = tb_screen_row((uint8_t)y);
        const mode0_colors_t *colors = tb_screen_row_colors((uint8_t)y);
        o->line[y] = esc_hash[tb_screen_row_size((uint8_t)y) & 3];
        for (int x = 0; x < vt_cols; x++)
        {
            xm_cell_t *c = &o->cell[y][x];
            const uint16_t a = row[x].attributes;
            const unsigned ul = (a & TERM_ATTR_UL_MASK) >> TERM_ATTR_UL_SHIFT;
            c->cp = vt_unicode(row[x].glyph);
            c->attr = (uint8_t)(((a & TERM_ATTR_ANY_BLINK) ? XM_ATTR_BLINK : 0) |
                                (ul ? XM_ATTR_UL : 0) | (ul == TERM_UL_DOUBLE ? XM_ATTR_DUL : 0) |
                                ((a & TERM_ATTR_STRIKE) ? XM_ATTR_STRIKE : 0) |
                                ((a & TERM_ATTR_OVERLINE) ? XM_ATTR_OVERLINE : 0) |
                                ((row[x].glyph >= 0x100 && !mode0_glyph_is_dec(row[x].glyph))
                                     ? XM_ATTR_ITALIC
                                     : 0));
            uint16_t both[2];
            mode0_cell_shows(&row[x], &colors[x], &both[0], &both[1]);
            uint32_t out[2];
            for (int k = 0; k < 2; k++)
            {
                const uint16_t col = both[k];
                if (col & SCANVIDEO_ALPHA_MASK)
                    out[k] = XM_RGB5(SCANVIDEO_R5_FROM_PIXEL(col), SCANVIDEO_G5_FROM_PIXEL(col),
                                     SCANVIDEO_B5_FROM_PIXEL(col));
                else
                {
                    const unsigned code = col >> MODE0_ENTRY_SHIFT;
                    const unsigned e = (code & MODE0_CODE_DEFAULT)
                                           ? MODE0_DEFAULT_FG + (code & 1)
                                           : code & 0xFF;
                    out[k] = (code & MODE0_CODE_FAINT) ? e | XM_FAINT : e;
                }
            }
            c->fg = out[0];
            c->bg = out[1];
            c->known = true;
        }
    }
}

static void vt_color_name(uint32_t c, char *out, size_t size)
{
    const char *faint = (c & XM_FAINT) && !(c & XM_RGB) ? " faint" : "";
    if (c & XM_RGB)
        snprintf(out, size, "#%02X%02X%02X", (c >> 10 & 31) << 3, (c >> 5 & 31) << 3,
                 (c & 31) << 3);
    else if ((c & ~XM_FAINT) == XM_DEFAULT_FG)
        snprintf(out, size, "fg%s", faint);
    else if ((c & ~XM_FAINT) == XM_DEFAULT_BG)
        snprintf(out, size, "bg%s", faint);
    else
        snprintf(out, size, "%u%s", c & ~XM_FAINT, faint);
}

static void vt_cell_name(const xm_cell_t *c, bool look, char *out, size_t size)
{
    char glyph[8] = {0}, fg[24], bg[24];
    vt_utf8(c->cp, glyph);
    if (!look)
    {
        snprintf(out, size, "'%s'", glyph);
        return;
    }
    vt_color_name(c->fg, fg, sizeof fg);
    vt_color_name(c->bg, bg, sizeof bg);
    snprintf(out, size, "'%s' %s/%s%s%s%s%s%s%s", glyph, fg, bg,
             c->attr & XM_ATTR_UL ? (c->attr & XM_ATTR_DUL ? " dul" : " ul") : "",
             c->attr & XM_ATTR_BLINK ? " blink" : "", c->attr & XM_ATTR_STRIKE ? " strike" : "",
             c->attr & XM_ATTR_ITALIC ? " italic" : "",
             c->attr & XM_ATTR_OVERLINE ? " overline" : "", "");
}

/* A row as text, of the cells a line of its size shows. */
static void vt_xrow(const xm_screen_t *s, int y, char *out, size_t size)
{
    int cols = s->line[y] == 5 || !s->line[y] ? s->cols : s->cols / 2;
    size_t o = 0;
    for (int x = 0; x < cols && o + 8 < size; x++)
        o += vt_utf8(s->cell[y][x].cp, out + o);
    while (o && out[o - 1] == ' ')
        o--;
    out[o] = 0;
}

static void vt_compare(const char *name)
{
    static xm_screen_t theirs, ours;
    if (!xm_snap(&theirs))
    {
        fprintf(stderr, "vtrun: no screen from xterm at %s\n", name);
        exit(2);
    }
    vt_ours(&ours);
    int text = 0, look = 0, lines = 0;
    uint32_t digest = 0;
    char detail[64][160];
    int details = 0;
    for (int y = 0; y < ours.rows; y++)
    {
        if (theirs.line[y] && ours.line[y] != theirs.line[y])
        {
            lines++;
            const uint8_t pair[3] = {(uint8_t)y, ours.line[y], theirs.line[y]};
            digest = host_crc32(digest, pair, sizeof pair);
        }
        /* A wide line shows half its cells. */
        const int cols = ours.line[y] == 5 && theirs.line[y] != 6 && theirs.line[y] != 3 &&
                                 theirs.line[y] != 4
                             ? ours.cols
                             : ours.cols / 2;
        for (int x = 0; x < cols; x++)
        {
            const xm_cell_t *a = &ours.cell[y][x], *b = &theirs.cell[y][x];
            const bool t = a->cp != b->cp;
            /* Past the end of a line xterm printed, only its text is known.
             * A blank's ink shows only in a line through it. */
            const bool ink = a->cp != ' ' || ((a->attr | b->attr) & (XM_ATTR_UL | XM_ATTR_STRIKE |
                                                                      XM_ATTR_OVERLINE));
            const bool l = b->known && ((ink && a->fg != b->fg) || a->bg != b->bg ||
                                        a->attr != b->attr);
            if (!t && !l)
                continue;
            text += t;
            look += !t;
            const struct
            {
                uint8_t y, x;
                uint32_t cp_a, cp_b, fg_a, fg_b, bg_a, bg_b;
                uint8_t attr_a, attr_b;
            } d = {(uint8_t)y, (uint8_t)x, a->cp, b->cp, a->fg, b->known ? b->fg : 0,
                   a->bg, b->known ? b->bg : 0, a->attr, b->known ? b->attr : 0};
            digest = host_crc32(digest, &d, sizeof d);
            if (details < 64)
            {
                char an[64], bn[64];
                vt_cell_name(a, l, an, sizeof an);
                vt_cell_name(b, l, bn, sizeof bn);
                snprintf(detail[details++], sizeof detail[0], "  %2d,%-2d ours %s, xterm %s", y,
                         x, an, bn);
            }
        }
    }
    const bool cursor = ours.cursor_x != theirs.cursor_x || ours.cursor_y != theirs.cursor_y;
    const bool reverse = ours.reverse != theirs.reverse;
    if (cursor)
    {
        const int c[4] = {ours.cursor_x, ours.cursor_y, theirs.cursor_x, theirs.cursor_y};
        digest = host_crc32(digest, c, sizeof c);
    }
    if (reverse)
        digest = host_crc32(digest, &ours.reverse, 1);
    if (!text && !look && !lines && !cursor && !reverse)
        return;

    char summary[256];
    int sl = snprintf(summary, sizeof summary, "%s %08X", name, digest);
    if (text)
        sl += snprintf(summary + sl, sizeof summary - sl, " text %d", text);
    if (look)
        sl += snprintf(summary + sl, sizeof summary - sl, " look %d", look);
    if (lines)
        sl += snprintf(summary + sl, sizeof summary - sl, " lines %d", lines);
    if (cursor)
        sl += snprintf(summary + sl, sizeof summary - sl, " cursor %d,%d xterm %d,%d",
                       ours.cursor_x, ours.cursor_y, theirs.cursor_x, theirs.cursor_y);
    if (reverse)
        snprintf(summary + sl, sizeof summary - sl, " reverse %s", ours.reverse ? "ours" : "xterm");
    fprintf(vt_xterm, "%s\n", summary);
    fflush(vt_xterm);
    if (vt_known_difference(name, digest))
        return;
    vt_xdiffer++;
    fprintf(stderr, "vtrun: differs from xterm: %s\n", summary);

    char path[512];
    snprintf(path, sizeof path, "%s/xterm", vt_outdir);
    mkdir(path, 0777);
    snprintf(path, sizeof path, "%s/xterm/%s.txt", vt_outdir, name);
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "%s\nafter the first %ld bytes of stream.bin\n\n", summary, vt_streamed);
    char a[1024], b[1024];
    for (int y = 0; y < ours.rows; y++)
    {
        vt_xrow(&ours, y, a, sizeof a);
        vt_xrow(&theirs, y, b, sizeof b);
        fprintf(f, "%c%2d ours  |%s\n", strcmp(a, b) ? '*' : ' ', y, a);
        if (strcmp(a, b))
            fprintf(f, "    xterm |%s\n", b);
    }
    if (lines)
    {
        fprintf(f, "\nline sizes (ESC # n), ours/xterm:");
        for (int y = 0; y < ours.rows; y++)
            if (theirs.line[y] && ours.line[y] != theirs.line[y])
                fprintf(f, " row %d %d/%d", y, ours.line[y], theirs.line[y]);
        fprintf(f, "\n");
    }
    if (details)
    {
        fprintf(f, "\ncells (row,col)%s:\n", details == 64 ? ", the first 64" : "");
        for (int i = 0; i < details; i++)
            fprintf(f, "%s\n", detail[i]);
    }
    fclose(f);
}

static void vt_save(const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.txt", vt_outdir, name);
    FILE *f = fopen(path, "w");
    if (f)
    {
        char line[1024];
        for (int y = 0; y < vt_rows; y++)
        {
            vt_row(y, line, sizeof line);
            fprintf(f, "%s\n", line);
        }
        fclose(f);
    }
    int w, h;
    vga_canvas_size(&w, &h);
    snprintf(path, sizeof path, "%s/%s.png", vt_outdir, name);
    vga_draw(vt_fb);
    png_write(path, w, h, vt_fb);
}

/* Records the screen as it is. */
static void vt_shot(const char *name)
{
    vt_settle();
    tb_screen_t v;
    tb_screen(&v);
    vt_last = vt_cells();
    vt_fed = 0;
    /* golden.txt has a cursor with a wrap pending one column past its own. */
    char line[160];
    snprintf(line, sizeof line, "%s %08X cursor %d,%d style %d%s", name, vt_digest(),
             v.cursor_x + v.cursor_pending, v.cursor_y, v.cursor_style,
             v.cursor_enabled ? "" : " hidden");
    fprintf(vt_screens, "%s\n", line);
    fflush(vt_screens);
    bool ok = !vt_have_golden || vt_approved(name, line);
    if (!ok)
        vt_differ++;
    if (!vt_have_golden || !ok)
        vt_save(name);
    if (vt_mirror)
        vt_compare(name);
}

static char vt_echo[256]; /* the bytes of the keys last typed */
static size_t vt_echo_len;

/* Whether vttest last asked for 132 columns (DECCOLM, CSI ? 3 h). The
 * terminal stays at 80, so those passes of a test show its layout cut short;
 * the index marks their screens for a person reviewing them. */
static bool vt_132;
static char vt_tail[5];


/* vttest clears the screen with CSI 2 J. A test that ends, or turns a page,
 * without waiting for a key is recorded just before its clear. Not when all
 * that came since the last recording is the tty's echo of the key the walk
 * pressed, which only moves the screen; nor when the screen is blank, is as it
 * was last recorded, or is the menu the walk is leaving. */
static int vt_clear_match;

static void vt_before_clear(void)
{
    if (vt_fed <= vt_echo_len + 2)
        return;
    vt_settle();
    if (vt_cells() == vt_last || vt_blank() || vt_screen_has("Enter choice number"))
        return;
    char name[96];
    snprintf(name, sizeof name, "%s-%02u", vt_path, ++vt_seq);
    vt_shot(name);
    vt_log("%s cleared%s\n", name, vt_132 ? " (132 columns)" : "");
}

static void vt_feed(const char *buf, size_t n)
{
    static const char clear[] = "\33[2J";
    size_t from = 0;
    vt_fed += n;
    for (size_t i = 0; i < n; i++)
    {
        memmove(vt_tail, vt_tail + 1, sizeof vt_tail - 1);
        vt_tail[sizeof vt_tail - 1] = buf[i];
        if (!memcmp(vt_tail, "\33[?3h", 5))
            vt_132 = true;
        else if (!memcmp(vt_tail, "\33[?3l", 5))
            vt_132 = false;
        if (buf[i] == clear[vt_clear_match])
            vt_clear_match++;
        else
            vt_clear_match = buf[i] == clear[0];
        if (vt_clear_match == 4)
        {
            /* Everything up to the J goes in first; the clear happens on it. */
            vt_term_write(buf + from, i - from);
            vt_fed -= n - i + 3; /* the clear and what follows it are not before it */
            vt_before_clear();
            vt_fed += n - i + 3;
            from = i;
            vt_clear_match = 0;
        }
    }
    vt_term_write(buf + from, n - from);
}

/* ------------------------------------------------------------------------
 * The walk */

#define VT_CHOICES 32 /* the most choices a menu has */

typedef struct
{
    char key[160];
    char id[24]; /* the title's number, "main", or m03 for the fourth menu met */
    int next;    /* the next choice to take, from 1, or of its script (vt_scripts) */
    int visits;
    uint32_t last; /* the cells of its last recording */
    uint8_t taken[VT_CHOICES]; /* the times each choice was taken */
} vt_menu_t;

#define VT_MENUS 256
static vt_menu_t vt_menus[VT_MENUS];
static int vt_menu_count;
static vt_menu_t *vt_left; /* the menu the walk last chose on */

/* Choices never taken, by the words of their label: they wait for keys a
 * script cannot judge, or change how the rest of the run behaves. */
static const char *const vt_skip[] = {
    "Modify test-parameters", /* changes the rest of the run */
    "Swedish national layout", /* the walk types on a US keyboard, choice 0 */
    "User-Defined Keys",      /* function keys */
    "Tektronix 4014",         /* a graphics terminal xterm can switch to */
    /* Past what the console aims for: the VT420 and the xterm features a
     * program reaches for. */
    "VT520 features",
    /* DECRQCRA, which the console leaves out: a checksum of the screen is a
     * way to read it back, which a host must not have. vttest waits for an
     * answer that never comes. */
    "Checksum of Rectangular Area",
    /* Keeps the alternate-screen tests of 11.8.7 in color. */
    "Do not color after switching",
};

/* The menus whose choices the walk takes in an order of its own, as their
 * number has them, until 0. The mouse's: focus events on, then the five
 * tests of the mouse's events in each of the five forms of report, each
 * form after the last, and the DEC locator's in the first, then the normal
 * form and no focus events back. vttest marks the locator's cells as
 * pixels while the pixel form is chosen, and answers highlight tracking's
 * press in pixels with the pixel as the cell, then marks the cells of its
 * report as pixels over its own prompt, so the locator goes in the normal
 * form and highlight tracking is left out in pixels. */
static const struct
{
    const char *id;
    const int *choices;
} vt_scripts[] = {
    {"11.8.5", (const int[]){2, 3, 4, 5, 6, 7, 8, 1, 3, 4, 5, 6, 7, 1, 3, 4, 5, 6, 7, 1, 3, 4, 5,
                              6, 7, 1, 3, 4, 6, 7, 1, 2, 0}},
};

static const int *vt_script(const char *id)
{
    for (size_t i = 0; i < sizeof vt_scripts / sizeof vt_scripts[0]; i++)
        if (!strcmp(vt_scripts[i].id, id))
            return vt_scripts[i].choices;
    return NULL;
}

/* What to type where a test asks for more than Return, found by a prompt on
 * the screen; the first row that matches wins. A step is the keys typed at
 * one wait, as vt_type takes them, and locks are the keyboard's locks while
 * they are typed. A test that reads one key at a time and gives each a line gets
 * one key a step. Only the screen before the last step is recorded, which
 * shows what the steps before it did, and the last step is typed again at
 * every wait after it. A step that begins with + has the screen before it
 * recorded too, for what a step before it showed only until the next.
 *
 * The mouse is in steps too, as words: @C,R moves the pointer over column C
 * of row R of the console, from 1; press-left, release-left and the same of
 * middle and right are its buttons; wheel-up and wheel-down turn its wheel
 * a notch; focus-out and focus-in are the window losing and gaining the
 * focus. */
typedef struct
{
    const char *prompt;
    uint8_t locks;
    const char *const *steps;
} vt_answer_t;

#define VT_STEPS(...) ((const char *const[]){__VA_ARGS__, NULL})

/* The keys of a PC keyboard that the VT100 keyboard has too: Ctrl-Backspace
 * for its BS and Ctrl-J for its LINE FEED. */
static const char *const vt_layout_keys[] = {
    "esc 1234567890-=` ctrl-backspace !@#$%^&*()_+~ "
    "tab qwertyuiop[] backspace QWERTYUIOP{} "
    "asdfghjkl;'\\ ASDFGHJKL:\"| "
    "zxcvbnm,./ ctrl-j ZXCVBNM<>? space",
    "return", NULL};

static const char *const vt_cursor_keys[] = {"up", "down", "left", "right", "tab", NULL};

/* The comma is the key some keypads have for it, which types a comma in
 * numeric mode where a PC's plus would not. */
static const char *const vt_keypad_keys[] = {
    "f1", "f2", "f3", "f4", "kp7", "kp8", "kp9", "kp-", "kp4", "kp5", "kp6",
    "kp,", "kp1", "kp2", "kp3", "kp0", "kp.", "kpenter", "tab", NULL};

/* The keys of a PC keyboard that a VT220's function and editing keys are. */
static const char *const vt_function_keys[] = {
    "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12",
    "insert", "delete", "home", "end", "pageup", "pagedown",
    "up", "down", "left", "right", "tab", NULL};

static const char *const vt_control_keys[] = {
    "ctrl-space ctrl-a ctrl-b ctrl-c ctrl-d ctrl-e ctrl-f ctrl-g ctrl-h ctrl-i "
    "ctrl-j ctrl-k ctrl-l ctrl-m ctrl-n ctrl-o ctrl-p ctrl-q ctrl-r ctrl-s "
    "ctrl-t ctrl-u ctrl-v ctrl-w ctrl-x ctrl-y ctrl-z ctrl-[ ctrl-\\ ctrl-] "
    "ctrl-6 ctrl-/",
    "ctrl-space ctrl-a ctrl-b ctrl-c ctrl-d ctrl-e ctrl-f ctrl-g ctrl-h ctrl-i "
    "ctrl-j ctrl-k ctrl-l ctrl-m ctrl-n ctrl-o ctrl-p ctrl-q ctrl-r ctrl-s "
    "ctrl-t ctrl-u ctrl-v ctrl-w ctrl-x ctrl-y ctrl-z ctrl-[ ctrl-\\ ctrl-] "
    "ctrl-6 ctrl-/",
    "backspace", NULL};

/* The tests of the mouse's menus, by their titles, which those with more
 * words come before. Each marks where the reports put the pointer. */
static const vt_answer_t vt_answers[] = {
    {"X10 Mouse Compatibility", 0,
     VT_STEPS("@10,8 press-left release-left", "@20,9 press-middle release-middle",
              "@30,10 press-right release-right", "q")},
    /* The press is told; moved with it held, the release is a * where it
     * came up; the wheel is button 4 and 5; and the focus goes and comes
     * back, which the reports show as they came. */
    {"Normal Mouse Tracking", 0,
     VT_STEPS("@10,8 press-left", "@14,8 release-left", "@20,10 press-right release-right",
              "wheel-up", "wheel-down", "focus-out", "focus-in", "q")},
    /* vttest answers a press with the rows 10 to 20, and the drag shows the
     * highlight, which its release tells and ends: where it ends, when it
     * ends at the pointer from where vttest said; all of it, after a drag
     * back; and a drag past row 19 held to it. */
    {"Mouse Highlight Tracking", 0,
     VT_STEPS("@10,12 press-left", "@30,14", "+release-left", "@40,15 press-left", "@20,13",
              "+release-left", "@10,12 press-left", "@20,25", "+release-left", "q")},
    {"Mouse Any-Event Tracking", 0,
     VT_STEPS("@10,8", "@12,8", "press-left", "@16,9", "release-left", "q")},
    {"Mouse Button-Event Tracking", 0,
     VT_STEPS("@10,8", "press-left", "@16,9", "release-left", "q")},
    /* The locator's: one-shot tells the press and no more; repeated each
     * press and release; the rectangle, from the pointer where the walk
     * left it, as the locator leaves it; unfiltered, a cell's rectangle
     * each time, as it leaves each. q asks for the position. */
    {"One-Shot (pixels)", 0, VT_STEPS("@10,8 press-left", "release-left", "q")},
    {"Repeated (pixels)", 0,
     VT_STEPS("@10,8 press-left", "release-left", "@20,12 press-right release-right", "q")},
    {"Filter Rectangle (unfiltered)", 0, VT_STEPS("@41,15", "@45,16", "q")},
    {"One-Shot", 0, VT_STEPS("@10,8 press-left", "release-left", "q")},
    {"Repeated", 0,
     VT_STEPS("@10,8 press-left", "release-left", "@20,12 press-right release-right", "q")},
    {"Filter Rectangle", 0, VT_STEPS("@40,25", "q")},
    {"Press 'q' to quit", 0, VT_STEPS("q")},
    {"Please enter the", 0, VT_STEPS("vttest return")}, /* a window title, a font name */
    /* A key held down: with auto repeat off the repeats are not typed. */
    {"Auto Repeat ON:", 0, VT_STEPS("a again again return")},
    {"Auto Repeat OFF:", 0, VT_STEPS("a again again return")},
    {"Press each key, both shifted and unshifted", 0, vt_layout_keys},
    {"<ANSI / Cursor key mode RESET>", 0, vt_cursor_keys},
    {"<ANSI / Cursor key mode SET>", 0, vt_cursor_keys},
    {"<VT52 Mode>", 0, vt_cursor_keys},
    /* NumLock is on for the numeric keypad and off for the application
     * keypad, which a keypad with NumLock on does not send. */
    {"<ANSI Numeric mode>", KEYBOARD_LED_NUMLOCK, vt_keypad_keys},
    {"<ANSI Application mode>", 0, vt_keypad_keys},
    {"<VT52 Numeric mode>", KEYBOARD_LED_NUMLOCK, vt_keypad_keys},
    {"<VT52 Application mode>", 0, vt_keypad_keys},
    {"Push each CTRL-key TWICE", 0, vt_control_keys},
    {"Press each function key. Finish with TAB.", 0, vt_function_keys},
    /* A key a wait, the same one twice to end */
    {"When you are done, press any key twice to quit.", KEYBOARD_LED_NUMLOCK,
     VT_STEPS("kp1", "kp1")},
    {"Press any keys, repeat to quit.", 0, VT_STEPS("a", "a")},
    {"Press the backspace key", 0, VT_STEPS("backspace")},
};

static const vt_answer_t *vt_answer; /* the row the walk is in */
static int vt_step;                  /* and the next step of it */

static int vt_slave = -1;
static int vt_stuck_ms = VT_STUCK_MS;

/* The pipes vtsleep.c's pauses talk to vtrun on, when it is preloaded. */
static int vt_pause_tell = -1, vt_pause_wait = -1;

/* Whether vttest sleeps: its state in /proc is S, a process waiting for
 * something. One that is running, or waiting only for a processor, is R. */
static bool vt_asleep(void)
{
    char path[64], line[512];
    snprintf(path, sizeof path, "/proc/%d/stat", (int)vt_pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    bool got = fgets(line, sizeof line, f);
    fclose(f);
    /* The state follows the name, which is in parentheses and may hold any
     * character, so it is found from the last of them. */
    const char *end = got ? strrchr(line, ')') : NULL;
    return end && end[1] == ' ' && end[2] == 'S';
}

/* Whether vttest waits for a key: it is blocked reading its terminal, and
 * nothing is there for it to read. Linux shows the call a process is in, and
 * vttest reads keys, and the terminal's replies, with read on fd 0.
 *
 * Being in the call is not enough. vttest reads a sequence a byte at a time,
 * and for a moment after a read has taken the last byte it is still in that
 * call with nothing left to read, about to go on with what it read. On a busy
 * machine that moment is long, and a key typed in it lands in the sequence
 * before it. A read that waits is asleep, and one that took a byte is not. */
static bool vt_reading(void)
{
    char path[64], line[256];
    snprintf(path, sizeof path, "/proc/%d/syscall", (int)vt_pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    long nr = -1;
    unsigned long fd = 1;
    bool ok = fgets(line, sizeof line, f) && sscanf(line, "%ld 0x%lx", &nr, &fd) == 2;
    fclose(f);
    int queued = 1;
    return ok && nr == SYS_read && fd == 0 && vt_asleep() && vt_taken() &&
           !ioctl(vt_slave, FIONREAD, &queued) && !queued;
}

/* Reads what vttest writes until it waits for a key, answering as the
 * terminal does. False once it is gone. vttest flushes its output before it
 * reads, so all of a screen is in when it waits, and a reply the terminal
 * gave is in its input until it reads it. Its pauses are sleeps, not reads,
 * so no key lands in one. */
static bool vt_drain(void)
{
    int quiet = 0;
    for (;;)
    {
        struct pollfd p[2] = {{.fd = vt_master, .events = POLLIN},
                              {.fd = vt_pause_tell, .events = POLLIN}};
        int k = poll(p, vt_pause_tell < 0 ? 1 : 2, VT_POLL_MS);
        if (k < 0 && errno == EINTR)
            continue;
        if (k > 0 && !(p[0].revents & (POLLIN | POLLHUP)) && (p[1].revents & POLLIN))
        {
            /* vttest pauses, with all it wrote before the pause written:
             * that is read and answered, and the pause ends. What it wrote
             * reaches this side of the pseudoterminal a moment after, so a
             * moment's quiet is waited for first. */
            struct pollfd again = {.fd = vt_master, .events = POLLIN};
            if (poll(&again, 1, 1) > 0)
                continue;
            char c;
            (void)!read(vt_pause_tell, &c, 1);
            (void)!write(vt_pause_wait, &c, 1);
            quiet = 0;
            continue;
        }
        if (k > 0)
        {
            char buf[4096];
            ssize_t n = read(vt_master, buf, sizeof buf);
            if (n <= 0)
                return false;
            vt_feed(buf, (size_t)n);
            quiet = 0;
            continue;
        }
        if (vt_reading())
        {
            /* vttest can write a screen and start to read between the poll
             * and this look at it, and what it wrote is then still to be
             * read here. Once it is seen reading it writes nothing more,
             * so one more look at the terminal settles it. */
            struct pollfd again = {.fd = vt_master, .events = POLLIN};
            if (poll(&again, 1, 0) > 0)
                continue;
            return true;
        }
        quiet += VT_POLL_MS;
        if (quiet >= vt_stuck_ms)
            return true;
    }
}

/* Waits for vttest to want a key, with the screen published and blinking
 * text lit. */
static bool vt_wait(void)
{
    if (!vt_drain())
        return false;
    vt_settle();
    return true;
}

/* The menu on the screen, if there is one: its key, its "Menu 11.1" title if
 * it has one and the text of its first choice, which together no two menus
 * share; its title's number, or "" without one; and its highest choice. */
static bool vt_menu_on_screen(char *key, size_t size, char *number, size_t nsize,
                              int *highest)
{
    char line[1024], first[256] = "";
    bool menu = false;
    number[0] = 0;
    tb_screen_t v;
    tb_screen(&v);
    for (int y = 0; y < vt_rows; y++)
    {
        vt_row(y, line, sizeof line);
        /* Only the prompt the cursor waits on: a test can leave an old
         * menu's prompt on the screen. */
        const char *p = strstr(line, "Enter choice number (0 - ");
        if (p && y == v.cursor_y)
        {
            *highest = atoi(p + strlen("Enter choice number (0 - "));
            menu = true;
        }
        const char *c = line;
        while (*c == ' ')
            c++;
        /* "1. Test..", or "1* Test.." for a test of a feature not set up */
        if (!first[0] && c[0] == '1' && (c[1] == '.' || c[1] == '*') && c[2] == ' ')
            snprintf(first, sizeof first, "%s", c + 3);
        const char *m = strstr(line, "Menu ");
        if (!number[0] && m && strchr(m, ':'))
        {
            size_t n = (size_t)(strchr(m, ':') - (m + 5));
            if (n < nsize)
            {
                memcpy(number, m + 5, n);
                number[n] = 0;
            }
        }
    }
    snprintf(key, size, "%s|%s", number, first);
    return menu && first[0];
}

static vt_menu_t *vt_menu(const char *key, const char *number)
{
    for (int i = 0; i < vt_menu_count; i++)
        if (!strcmp(vt_menus[i].key, key))
            return &vt_menus[i];
    /* A choice that only flips a setting, such as "Enable ERM", brings the
     * same menu straight back with that choice relabelled. It is the menu
     * the walk left, and the walk goes on from where it was there. */
    if (vt_left && !vt_seq && number[0] && !strncmp(vt_left->key, number, strlen(number)) &&
        vt_left->key[strlen(number)] == '|')
    {
        snprintf(vt_left->key, sizeof vt_left->key, "%s", key);
        return vt_left;
    }
    if (vt_menu_count == VT_MENUS)
        return NULL;
    vt_menu_t *m = &vt_menus[vt_menu_count];
    snprintf(m->key, sizeof m->key, "%s", key);
    if (!vt_menu_count)
        snprintf(m->id, sizeof m->id, "main");
    else if (!number[0])
        snprintf(m->id, sizeof m->id, "m%02d", vt_menu_count);
    else
    {
        /* vttest gives two menus the same number in places */
        snprintf(m->id, sizeof m->id, "%s", number);
        for (int i = 0; i < vt_menu_count; i++)
            if (!strcmp(vt_menus[i].id, m->id))
                snprintf(m->id, sizeof m->id, "%s.m%02d", number, vt_menu_count);
    }
    vt_menu_count++;
    m->next = 1;
    vt_log("%s is the menu of \"%s\"\n", m->id, key);
    return m;
}

/* The label of choice n on the menu on the screen. */
static bool vt_choice_label(int n, char *out, size_t size)
{
    char line[1024], tag[8];
    snprintf(tag, sizeof tag, "%d", n);
    size_t t = strlen(tag);
    for (int y = 0; y < vt_rows; y++)
    {
        vt_row(y, line, sizeof line);
        const char *c = line;
        while (*c == ' ')
            c++;
        if (!strncmp(c, tag, t) && (c[t] == '.' || c[t] == '*') && c[t + 1] == ' ')
        {
            snprintf(out, size, "%s", c + t + 2);
            return true;
        }
    }
    return false;
}

static bool vt_skipped(int choice)
{
    char label[256];
    if (!vt_choice_label(choice, label, sizeof label))
        return false;
    for (size_t i = 0; i < sizeof vt_skip / sizeof vt_skip[0]; i++)
        if (strstr(label, vt_skip[i]))
            return true;
    return false;
}

/* The keys with a name. kp0 to kp9 are computed. */
static const struct
{
    const char *name;
    uint8_t usage;
} vt_named[] = {
    {"return", HID_KEY_ENTER},
    {"esc", HID_KEY_ESCAPE},
    {"backspace", HID_KEY_BACKSPACE},
    {"tab", HID_KEY_TAB},
    {"space", HID_KEY_SPACE},
    {"up", HID_KEY_ARROW_UP},
    {"down", HID_KEY_ARROW_DOWN},
    {"left", HID_KEY_ARROW_LEFT},
    {"right", HID_KEY_ARROW_RIGHT},
    {"f1", HID_KEY_F1},
    {"f2", HID_KEY_F2},
    {"f3", HID_KEY_F3},
    {"f4", HID_KEY_F4},
    {"f5", HID_KEY_F5},
    {"f6", HID_KEY_F6},
    {"f7", HID_KEY_F7},
    {"f8", HID_KEY_F8},
    {"f9", HID_KEY_F9},
    {"f10", HID_KEY_F10},
    {"f11", HID_KEY_F11},
    {"f12", HID_KEY_F12},
    {"insert", HID_KEY_INSERT},
    {"delete", HID_KEY_DELETE},
    {"home", HID_KEY_HOME},
    {"end", HID_KEY_END},
    {"pageup", HID_KEY_PAGE_UP},
    {"pagedown", HID_KEY_PAGE_DOWN},
    {"kp-", HID_KEY_KEYPAD_SUBTRACT},
    {"kp,", HID_KEY_KEYPAD_COMMA},
    {"kp.", HID_KEY_KEYPAD_DECIMAL},
    {"kpenter", HID_KEY_KEYPAD_ENTER},
};

/* The key last pressed, which is the one that repeats. */
static uint8_t vt_held_usage, vt_held_modifier;

/* Presses a key of a US keyboard, or repeats the held one, and what it sends
 * goes to vttest as it would from the console. modifier is a HID modifier
 * byte. */
static void vt_press(uint8_t usage, uint8_t modifier, bool repeat)
{
    vt_held_usage = usage;
    vt_held_modifier = modifier;
    hostkey_press(usage, modifier, repeat, true);
    char buf[64];
    size_t n;
    while ((n = com_read(COM_SOURCE_KEYBOARD, buf, sizeof buf)) > 0)
    {
        vt_reply(buf, n);
        if (n > sizeof vt_echo - vt_echo_len)
            n = sizeof vt_echo - vt_echo_len;
        memcpy(vt_echo + vt_echo_len, buf, n);
        vt_echo_len += n;
    }
}

/* The mouse: the cell the pointer is over, from 1, and the buttons held.
 * The walk leaves it in the middle of the console when it chooses a test of
 * the mouse's menus, where no mode of the mouse is set. */
static int vt_mouse_col = 40, vt_mouse_row = 15;
static uint8_t vt_mouse_buttons;

static void vt_mouse_point(void)
{
    tablet_host_pointer((vt_mouse_col - 1) * 8 + 3, (vt_mouse_row - 1) * 16 + 7,
                        vt_mouse_buttons, true);
    vt_mouse_sends();
}

/* A word of the mouse's (vt_answer_t), done: false for a word that is not. */
static bool vt_mouse(const char *word, size_t len)
{
    static const struct
    {
        const char *name;
        uint8_t button;
    } buttons[] = {{"left", TABLET_FLAG_LEFT}, {"middle", TABLET_FLAG_MIDDLE},
                   {"right", TABLET_FLAG_RIGHT}};
    char w[32];
    if (len >= sizeof w)
        return false;
    memcpy(w, word, len);
    w[len] = 0;
    int col, row;
    char end;
    if (sscanf(w, "@%d,%d%c", &col, &row, &end) == 2)
    {
        vt_mouse_col = col;
        vt_mouse_row = row;
        vt_mouse_point();
        return true;
    }
    for (size_t i = 0; i < sizeof buttons / sizeof buttons[0]; i++)
    {
        const bool press = !strncmp(w, "press-", 6) && !strcmp(w + 6, buttons[i].name);
        const bool release = !strncmp(w, "release-", 8) && !strcmp(w + 8, buttons[i].name);
        if (press || release)
        {
            if (press)
                vt_mouse_buttons |= buttons[i].button;
            else
                vt_mouse_buttons &= (uint8_t)~buttons[i].button;
            vt_mouse_point();
            return true;
        }
    }
    if (!strcmp(w, "wheel-up") || !strcmp(w, "wheel-down"))
    {
        tablet_host_wheel(w[6] == 'u' ? 1 : -1, 0);
        vt_mouse_sends();
        return true;
    }
    if (!strcmp(w, "focus-out") || !strcmp(w, "focus-in"))
    {
        vterm_focus(w[6] == 'i');
        vt_mouse_sends();
        return true;
    }
    return false;
}

/* Types keys, a list of words: a key's name, "again" for the last key
 * repeating while it is held, a word of the mouse's, or text, a character
 * a key. ctrl- in front of a name or a character holds Ctrl. locks are the
 * keyboard's locks. */
static void vt_type(const char *keys, uint8_t locks)
{
    keyboard_set_locks(locks);
    while (*keys)
    {
        size_t len = strcspn(keys, " ");
        const char *word = keys;
        keys += len;
        keys += *keys == ' ';
        if (vt_mouse(word, len))
            continue;
        if (len == 5 && !memcmp(word, "again", 5))
        {
            if (vtkey_repeats())
                vt_press(vt_held_usage, vt_held_modifier, true);
            continue;
        }
        uint8_t held = 0;
        if (len > 5 && !memcmp(word, "ctrl-", 5))
        {
            held |= KEYBOARD_MODIFIER_LEFTCTRL;
            word += 5;
            len -= 5;
        }
        if (len == 3 && !memcmp(word, "kp", 2) && word[2] >= '0' && word[2] <= '9')
        {
            vt_press(word[2] == '0' ? HID_KEY_KEYPAD_0 : (uint8_t)(HID_KEY_KEYPAD_1 + word[2] - '1'),
                     held, false);
            continue;
        }
        size_t i = 0;
        while (i < sizeof vt_named / sizeof vt_named[0] &&
               (strlen(vt_named[i].name) != len || memcmp(word, vt_named[i].name, len)))
            i++;
        if (i < sizeof vt_named / sizeof vt_named[0])
        {
            vt_press(vt_named[i].usage, held, false);
            continue;
        }
        for (i = 0; i < len; i++)
        {
            bool shift;
            uint8_t usage = hostkey_us_key(word[i], &shift);
            if (!usage)
            {
                fprintf(stderr, "vtrun: no key types '%c'\n", word[i]);
                exit(2);
            }
            vt_press(usage, shift ? held | KEYBOARD_MODIFIER_LEFTSHIFT : held, false);
        }
    }
}

static void vt_send(const char *name, const char *keys, uint8_t locks, const char *what)
{
    vt_log("%s %s%s\n", name, what, vt_132 ? " (132 columns)" : "");
    vt_echo_len = 0;
    vt_type(keys, locks);
}

static FILE *vt_open(const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", vt_outdir, name);
    FILE *f = fopen(path, "w");
    if (!f)
    {
        fprintf(stderr, "vtrun: cannot write %s\n", path);
        exit(2);
    }
    return f;
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s OUTDIR [vttest arguments]\n", argv[0]);
        return 2;
    }
    vt_outdir = argv[1];
    if (mkdir(vt_outdir, 0777) && errno != EEXIST)
    {
        fprintf(stderr, "vtrun: cannot make %s\n", vt_outdir);
        return 2;
    }
    if (getenv("VTGOLDEN"))
        vt_load_golden(getenv("VTGOLDEN"));
    vt_index = vt_open("index.txt");
    vt_screens = vt_open("screens.txt");

    vt_term_init();
    if (getenv("VTXTERM"))
    {
        /* VTXTERM names the xterm, or is 1 for the one on the PATH. */
        const char *xterm = getenv("VTXTERM");
        if (!xm_start(strcmp(xterm, "1") ? xterm : "xterm", vt_rows, vt_cols, vt_outdir))
            return 2;
        vt_xterm = vt_open("xterm.txt");
        vt_stream = vt_open("stream.bin");
        vt_mirror = true;
        vt_term_write("\33c", 2);
        if (getenv("VTXKNOWN"))
            vt_load_known(getenv("VTXKNOWN"));
    }
    struct winsize ws = {.ws_row = (unsigned short)vt_rows, .ws_col = (unsigned short)vt_cols};
    int tell[2] = {-1, -1}, wait[2] = {-1, -1};
    const char *preload = getenv("VTSLEEP");
    if (preload && (pipe(tell) || pipe(wait)))
    {
        perror("vtrun: pipe");
        return 2;
    }
    pid_t pid = forkpty(&vt_master, NULL, NULL, &ws);
    if (pid < 0)
    {
        perror("vtrun: forkpty");
        return 2;
    }
    if (pid == 0)
    {
        char geometry[32];
        snprintf(geometry, sizeof geometry, "%dx%d.%d", vt_rows, vt_cols, vt_cols);
        char *args[16] = {"vttest"};
        int n = 1;
        for (int i = 2; i < argc && n < 14; i++)
            args[n++] = argv[i];
        args[n++] = geometry;
        args[n] = NULL;
        setenv("TERM", "vt102", 1);
        if (preload)
        {
            char fds[32];
            snprintf(fds, sizeof fds, "%d,%d", tell[1], wait[0]);
            setenv("VTSLEEP_FDS", fds, 1);
            setenv("LD_PRELOAD", preload, 1);
            close(tell[0]);
            close(wait[1]);
        }
        const char *vttest = getenv("VTTEST");
        if (getenv("VTCMD"))
        {
            setenv("TERM", "xterm-256color", 1);
            execl("/bin/sh", "sh", "-c", getenv("VTCMD"), (char *)NULL);
            _exit(127);
        }
        args[0] = (char *)(vttest ? vttest : "vttest");
        execvp(args[0], args);
        _exit(127);
    }

    vt_pid = pid;
    if (preload)
    {
        close(tell[1]);
        close(wait[0]);
        vt_pause_tell = tell[0];
        vt_pause_wait = wait[1];
    }
    vt_slave = open(ptsname(vt_master), O_RDWR | O_NOCTTY);
    if (vt_slave < 0)
    {
        perror("vtrun: the pty's terminal");
        return 2;
    }
    int steps = 0;
    int status = 0;
    time_t start = time(NULL);
    if (getenv("VTCMD"))
    {
        /* Another program than vttest, which may never block in a read:
         * a screen is recorded when it has been quiet a while, and the
         * next step of VTKEYS, steps between |, is typed. */
        vt_stuck_ms = 400;
        const char *keys = getenv("VTKEYS");
        while (vt_wait())
        {
            char name[32], step[512];
            snprintf(name, sizeof name, "cmd-%02d", ++steps);
            vt_shot(name);
            if (!keys || !*keys)
                break;
            size_t len = strcspn(keys, "|");
            snprintf(step, sizeof step, "%.*s", (int)len, keys);
            keys += len + (keys[len] == '|');
            vt_send(name, step, 0, step);
        }
        goto done;
    }
    while (steps++ < VT_STEP_LIMIT && vt_wait())
    {
        if (time(NULL) - start > VT_TIME_LIMIT)
        {
            vt_log("time limit, at %s\n", vt_path);
            status = 1;
            break;
        }
        char name[96], key[160], number[24];
        int highest = 0;
        if (vt_menu_on_screen(key, sizeof key, number, sizeof number, &highest))
        {
            vt_menu_t *m = vt_menu(key, number);
            if (!m)
            {
                vt_log("too many menus\n");
                status = 1;
                break;
            }
            /* A menu is recorded when it looks different from the last time,
             * which is the first time and after a test changes a setting it
             * shows. */
            vt_settle();
            uint32_t now = vt_cells();
            if (!m->visits || now != m->last)
            {
                snprintf(name, sizeof name, "%s-menu-%02d", m->id, ++m->visits);
                vt_shot(name);
                m->last = now;
            }
            else
                snprintf(name, sizeof name, "%s-menu", m->id);
            vt_last = now;
            const int *script = vt_script(m->id);
            int choice;
            if (script)
                choice = script[m->next - 1] ? script[m->next++ - 1] : 0;
            else
            {
                while (m->next <= highest && vt_skipped(m->next))
                    m->next++;
                choice = m->next <= highest ? m->next++ : 0;
            }
            /* The mouse's tests start with the pointer in the middle of the
             * console and its buttons up. */
            if (!strncmp(m->id, "11.8.5", 6))
            {
                vt_mouse_col = 40;
                vt_mouse_row = 15;
                vt_mouse_buttons = 0;
                vt_mouse_point();
            }
            char keys[24], what[64];
            snprintf(keys, sizeof keys, "%d return", choice);
            snprintf(what, sizeof what, "choice %d", choice);
            vt_send(name, keys, 0, what);
            const unsigned taken = choice < VT_CHOICES ? ++m->taken[choice] : 1;
            if (taken > 1)
                snprintf(vt_path, sizeof vt_path, "%s_%d.%u", m->id, choice, taken);
            else
                snprintf(vt_path, sizeof vt_path, "%s_%d", m->id, choice);
            vt_seq = 0;
            vt_left = m;
            vt_answer = NULL;
            if (!strcmp(m->id, "main") && choice == 0)
                break;
        }
        else
        {
            if (vt_seq >= VT_TEST_LIMIT)
            {
                vt_log("%s never came back to its menu\n", vt_path);
                status = 1;
                break;
            }
            /* "Push <RETURN>" where the cursor waits wins over a prompt left
             * higher on the screen. */
            const vt_answer_t *answer = NULL;
            tb_screen_t v;
            tb_screen(&v);
            char row[1024];
            vt_row(v.cursor_y, row, sizeof row);
            for (size_t i = 0; !strstr(row, "Push <RETURN>") &&
                               i < sizeof vt_answers / sizeof vt_answers[0]; i++)
                if (vt_screen_has(vt_answers[i].prompt))
                {
                    answer = &vt_answers[i];
                    break;
                }
            if (answer != vt_answer)
            {
                vt_answer = answer;
                vt_step = 0;
            }
            const char *keys = "return";
            uint8_t locks = 0;
            bool last = true;
            if (answer)
            {
                keys = answer->steps[vt_step];
                locks = answer->locks;
                last = !answer->steps[vt_step + 1];
                if (!last)
                    vt_step++;
            }
            const bool shot = last || keys[0] == '+';
            keys += keys[0] == '+';
            if (shot)
            {
                snprintf(name, sizeof name, "%s-%02u", vt_path, ++vt_seq);
                vt_shot(name);
            }
            else
                snprintf(name, sizeof name, "%s", vt_path);
            vt_send(name, keys, locks, keys);
        }
    }
    if (steps > VT_STEP_LIMIT)
    {
        vt_log("step limit\n");
        status = 1;
    }
done:
    kill(pid, SIGKILL);
    close(vt_slave);
    close(vt_master);
    waitpid(pid, NULL, 0);
    fclose(vt_index);
    fclose(vt_screens);
    for (int i = 0; i < vt_golden_count; i++)
        if (!vt_golden[i].seen)
        {
            fprintf(stderr, "vtrun: %s is gone\n", vt_golden[i].line);
            vt_differ++;
        }
    if (vt_mirror)
    {
        xm_stop();
        fclose(vt_xterm);
        fclose(vt_stream);
        for (int i = 0; i < vt_known_count; i++)
            if (!vt_known[i].seen)
            {
                fprintf(stderr, "vtrun: no longer differs from xterm: %s\n", vt_known[i].line);
                vt_xdiffer++;
            }
        if (vt_xdiffer)
        {
            fprintf(stderr, "vtrun: %d differences from xterm are not in %s; see %s/xterm\n",
                    vt_xdiffer, getenv("VTXKNOWN") ? getenv("VTXKNOWN") : "VTXKNOWN", vt_outdir);
            status = 1;
        }
    }
    if (vt_differ)
    {
        fprintf(stderr, "vtrun: %d screens differ from %s; they are saved in %s\n",
                vt_differ, getenv("VTGOLDEN"), vt_outdir);
        status = 1;
    }
    return status;
}
