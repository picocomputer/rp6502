/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* xcheck: the console's terminal as the Pico VGA keeps it, in mode 0's
 * cells with a spare row and its jobs left pending (TERM_JOB_ROWS), or as
 * the RIA keeps it, in text cells with none and its jobs done at once
 * (TERM_CELL_TEXT, TERM_SPARE_ROWS 0). The two builds are fed the same
 * bytes, and the same changes of height and resets in the order
 * ria/sys/vga.c replays them, with smooth scrolls moving and holding bytes
 * as on a connected VGA, and jobs done a few passes of the main loop at a
 * time as on the Pico VGA. At points along the way a build prints a digest of
 * what both sizes' two screens hold: the text of each row in view and of
 * each row above them still in the ring, whether each is used, and whether
 * it wrapped on its own. The text is cell_text's, which is the same for
 * both kinds, so the two print the same digests. xcheck.py runs both and
 * holds them against each other.
 *
 *   xcheck fuzz SEED STEPS [POINT]   a seeded stream of sequences
 *   xcheck file PATH SEED [POINT]    the bytes of a file, the seed's changes
 *                                    of height between its chunks
 *
 * Each point prints a line, its number and its digest. With POINT, the
 * build prints all it holds at that point instead, and stops there. */

#include "core/aud/bel.h"
#include "core/com/com.h"
#include "core/term/draw.h"
#include "core/term/grid.h"
#include "core/term/screen.h"
#include "core/term/vterm.h"
#include "core/vga/vga.h"
#include "host/host.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the machine around the terminal ---------------------------------- */

// A VGA is connected, and its frames are counted here: a byte a smooth
// scroll holds waits for frames to pass.
static uint32_t xc_frames;

unsigned long vga_frame_count(void) { return xc_frames; }
bool vga_connected(void) { return true; }
void com_reply(const char *s, size_t n) { (void)s, (void)n; }
void bel_term(void) {}
void draw_reset(void) {}
bool host_minimized(void) { return false; }

#if TERM_CELL_TEXT
static cell_t xc_cells[VTERM_CELLS];
#else
mode0_cell_t mode0_cells[MODE0_CELLS];
mode0_colors_t mode0_colors[MODE0_CELLS];
#define xc_cells mode0_cells
#endif

/* ---- what the screens hold -------------------------------------------- */

static char xc_out[1 << 18];
static size_t xc_len;

static void xc_print(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(xc_out + xc_len, sizeof xc_out - xc_len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof xc_out - xc_len)
    {
        fprintf(stderr, "xcheck: a point holds too much to print\n");
        exit(2);
    }
    xc_len += (size_t)n;
}

// Row y of a screen: its text, a cell_text word a column, whether it is
// used, and whether it wrapped on its own. Row y is the screen's slot, so
// rows - k is the k-th newest row above row 0; label is the row as a person
// counts it.
static void xc_row(screen_t *s, uint8_t y, int label)
{
    uint16_t text[80];
    const unsigned cols = screen_row_text(s, y, text);
    xc_print("%3d %c%c", label, screen_row_used(s, y) ? '#' : '.',
             screen_wrapped(s, y) ? '>' : '.');
    for (unsigned x = 0; x < cols; x++)
        xc_print(" %04x", text[x]);
    xc_print("\n");
}

static void xc_point(void)
{
    xc_len = 0;
    const vterm_size_t *const sizes[] = {&vterm_40, &vterm_80};
    for (int n = 0; n < 2; n++)
    {
        term_grid_t *g = (term_grid_t *)&sizes[n]->grid;
        for (int b = 0; b < 2; b++)
        {
            screen_t *s = &g->screen[b];
            xc_print("%u columns, %s, %u rows, %u above\n", g->width,
                     b ? "alternate" : "primary", g->height, s->hidden);
            for (int k = s->hidden; k > 0; k--)
                xc_row(s, (uint8_t)(s->rows - k), -k);
            for (int y = 0; y < g->height; y++)
                xc_row(s, (uint8_t)y, y);
        }
    }
}

static unsigned xc_points;
static long xc_want = -1;

// A point: its digest, or with xc_want this point all it holds.
static void xc_check(void)
{
    term_job_finish(&vterm);
    const unsigned point = xc_points++;
    if (xc_want >= 0 && point != (unsigned long)xc_want)
        return;
    xc_point();
    if (xc_want >= 0)
    {
        fwrite(xc_out, 1, xc_len, stdout);
        exit(0);
    }
    printf("%u %08x\n", point, (unsigned)host_crc32(0, xc_out, xc_len));
}

/* ---- the stream -------------------------------------------------------- */

static uint64_t xc_seed;

static uint32_t xc_rand(void)
{
    xc_seed ^= xc_seed << 13;
    xc_seed ^= xc_seed >> 7;
    xc_seed ^= xc_seed << 17;
    return (uint32_t)(xc_seed >> 32);
}

static unsigned xc_below(unsigned n)
{
    return xc_rand() % n;
}

// A byte the terminal holds while a smooth scroll moves waits for the
// frames that land it. Between two bytes the main loop makes none to three
// passes, in turn, and a job still pending when the byte comes is finished
// first, as draw.c does.
static void xc_byte(uint8_t b)
{
    static unsigned passes;
    while (term_holds(&vterm, b))
        xc_frames++;
    term_job_step(&vterm, (passes++ % 4) * TERM_JOB_ROWS);
    term_job_finish(&vterm);
    term_write(&vterm, b);
}

static void xc_say(const char *fmt, ...)
{
    char seq[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(seq, sizeof seq, fmt, ap);
    va_end(ap);
    for (int i = 0; i < n && i < (int)sizeof seq - 1; i++)
        xc_byte((uint8_t)seq[i]);
}

// A row or a column, mostly on the screen and now and then past it or
// left to its default.
static unsigned xc_row_n(void)
{
    return xc_below(8) ? 1 + xc_below(32) : xc_below(40);
}

static unsigned xc_col_n(void)
{
    return xc_below(8) ? 1 + xc_below(80) : xc_below(100);
}

static unsigned xc_count(void)
{
    return xc_below(4) ? 1 + xc_below(4) : xc_below(40);
}

/* What ria/sys/vga.c replays of the VGA's: mode 0 programmed on the
 * canvas, a change of display (the console placed for the old display and
 * the new, then the soft reset), a RIS, or a canvas, which chooses the size
 * showing. */
static void xc_replay(void)
{
    const bool forty = vterm_placement.cols == 40;
    switch (xc_below(8))
    {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
        vterm_place(forty ? 40 : 80, (uint8_t)(1 + xc_below(forty ? 30 : 32)), 0,
                    forty ? 8 : 16);
        break;
    case 5:
        vterm_canvas(80, true);
        vterm_place(80, xc_below(2) ? 30 : 32, 0, 16);
        vterm_place(80, xc_below(2) ? 30 : 32, 0, 16);
        vterm_RIS_no_clear();
        break;
    case 6:
        if (!xc_below(4))
            vterm_RIS();
        break;
    default:
        vterm_canvas(forty ? 80 : 40, false);
        break;
    }
}

static const char *const xc_sgr[] = {
    "0", "1", "2", "3", "4", "4:3", "5", "6", "7", "8", "9", "21", "22", "23", "24", "25",
    "27", "28", "29", "53", "55", "31", "42", "39", "49", "93", "104", "38;5;196",
    "48;5;17", "48;2;10;20;30", "58;5;2", "59", "0;7", "0;44", "1;5",
};

static void xc_text(void)
{
    const unsigned n = 1 + xc_below(60);
    for (unsigned i = 0; i < n; i++)
    {
        const unsigned k = xc_below(16);
        if (k < 11)
            xc_byte((uint8_t)(0x20 + xc_below(0x5F)));
        else if (k < 13)
            xc_byte(' ');
        else
            xc_byte((uint8_t)(0x80 + xc_below(0x80)));
    }
}

static void xc_sequence(void)
{
    static const char *const ctl[] = {"\r", "\n", "\r\n", "\b", "\t", "\33D", "\33E",
                                      "\33M", "\33" "6", "\33" "9", "\v", "\f"};
    static const char *const cs[] = {"\33(0", "\33(B", "\33(A", "\33(>", "\33)0",
                                     "\33*0", "\17", "\16", "\33N", "\33O", "\33n"};
    static const char *const modes[] = {"?6", "?7", "?5", "4", "?33", "?4", "?1049",
                                        "?47", "?1047", "?1048", "?69", "?3", "?40"};
    static const char *const sizes[] = {"\33#3", "\33#4", "\33#5", "\33#6"};
    const unsigned t = xc_below(14), b = xc_below(34), l = xc_below(80);
    switch (xc_below(40))
    {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
        xc_text();
        break;
    case 6:
    case 7:
    case 8:
        xc_say("%s", ctl[xc_below(sizeof ctl / sizeof *ctl)]);
        break;
    case 9:
    case 10:
        xc_say("\33[%u;%uH", xc_row_n(), xc_col_n());
        break;
    case 11:
        xc_say("\33[%u%c", xc_count(), "ABCDGdEF"[xc_below(8)]);
        break;
    case 12:
        if (xc_below(3))
            xc_say("\33[%u;%ur", t, b);
        else
            xc_say("\33[r");
        break;
    case 13:
        xc_say("\33[?69h\33[%u;%us", l, l + xc_below(80));
        break;
    case 14:
        xc_say("\33[%u %c", xc_count(), xc_below(2) ? '@' : 'A');
        break;
    case 15:
        xc_say("\33[%u%c", xc_count(), "LM@PXST"[xc_below(7)]);
        break;
    case 16:
        xc_say("\33[%u'%c", xc_count(), xc_below(2) ? '}' : '~');
        break;
    case 17:
        xc_say("\33[%u;%u;%u;%u;1;%u;%u;1$v", xc_row_n(), xc_col_n(), xc_row_n(), xc_col_n(),
               xc_row_n(), xc_col_n());
        break;
    case 18:
        xc_say("\33[%u;%u;%u;%u$%c", xc_row_n(), xc_col_n(), xc_row_n(), xc_col_n(),
               xc_below(2) ? 'z' : '{');
        break;
    case 19:
        xc_say("\33[%u;%u;%u;%u;%u$x", 0x20 + xc_below(0x5F), xc_row_n(), xc_col_n(),
               xc_row_n(), xc_col_n());
        break;
    case 20:
    case 21:
        xc_say("\33[%u;%u;%u;%u;%u$%c", xc_row_n(), xc_col_n(), xc_row_n(), xc_col_n(),
               "014578"[xc_below(6)] - '0', xc_below(2) ? 'r' : 't');
        break;
    case 22:
        xc_say("\33[%u*x", xc_below(3));
        break;
    case 23:
    {
        static const char *const guard[] = {"\33[1\"q", "\33[0\"q", "\33[2\"q", "\33V",
                                            "\33W"};
        xc_say("%s", guard[xc_below(5)]);
        break;
    }
    case 24:
    case 25:
        xc_say("\33[%s%u%c", xc_below(2) ? "?" : "", xc_below(4), xc_below(2) ? 'J' : 'K');
        break;
    case 26:
        xc_say("\33[%ub", xc_count());
        break;
    case 27:
    case 28:
        xc_say("\33[%s%c", modes[xc_below(sizeof modes / sizeof *modes)],
               xc_below(2) ? 'h' : 'l');
        break;
    case 29:
        xc_say("%s", sizes[xc_below(4)]);
        break;
    case 30:
        if (!xc_below(8))
            xc_say("\33#8");
        break;
    case 31:
    case 32:
    case 33:
        xc_say("\33[%sm", xc_sgr[xc_below(sizeof xc_sgr / sizeof *xc_sgr)]);
        break;
    case 34:
        xc_say("%s", cs[xc_below(sizeof cs / sizeof *cs)]);
        break;
    case 35:
        xc_say(xc_below(2) ? "\0337" : "\0338");
        break;
    case 36:
        xc_say(xc_below(8) ? "\33[s" : "\33[!p");
        break;
    case 37:
        if (!xc_below(16))
            xc_say("\33c");
        break;
    case 38:
        xc_replay();
        break;
    default:
        xc_frames += xc_below(20);
        break;
    }
}

static void xc_fuzz(unsigned steps)
{
    for (unsigned i = 0; i < steps; i++)
    {
        xc_sequence();
        if (i % 32 == 31)
            xc_check();
    }
    xc_check();
}

static void xc_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        fprintf(stderr, "xcheck: no %s\n", path);
        exit(2);
    }
    uint8_t chunk[2048];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
    {
        for (size_t i = 0; i < n; i++)
            xc_byte(chunk[i]);
        xc_check();
        if (!xc_below(4))
            xc_replay();
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 4 || (strcmp(argv[1], "fuzz") && strcmp(argv[1], "file")))
    {
        fprintf(stderr, "usage: xcheck fuzz SEED STEPS [POINT]\n"
                        "       xcheck file PATH SEED [POINT]\n");
        return 2;
    }
    const bool fuzz = !strcmp(argv[1], "fuzz");
    xc_seed = strtoull(argv[fuzz ? 2 : 3], NULL, 0) * 0x9E3779B97F4A7C15ull | 1;
    if (argc > 4)
        xc_want = strtol(argv[4], NULL, 0);
    vterm_init(xc_cells);
    if (fuzz)
        xc_fuzz((unsigned)strtoul(argv[3], NULL, 0));
    else
        xc_file(argv[2]);
    return 0;
}
