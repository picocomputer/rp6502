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
 * at; without it, every screen is. */

#include "core/com/com.h"
#include "core/com/manifold.h"
#include "core/hid/hostkey.h"
#include "core/hid/keyboard.h"
#include "core/hid/usage.h"
#include "core/hid/vtkey.h"
#include "core/sys/sys.h"
#include "core/term/cells.h"
#include "tb_screen.h"
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

static void vt_reply(const char *s, size_t n)
{
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

/* The DEC special graphics, 0x5F to 0x7E, as Unicode. */
static const uint32_t vt_dec[32] = {
    ' ', 0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0,
    0x00B1, 0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C,
    0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534,
    0x252C, 0x2502, 0x2A7D, 0x2A7E, 0x03C0, 0x2260, 0x00A3, 0x00B7,
};

/* The CP437 glyphs of the C0 codes, which term.c draws for the controls it
 * has no action for. */
static const uint32_t vt_cp437_low[32] = {
    ' ', 0x263A, 0x263B, 0x2665, 0x2666, 0x2663, 0x2660, 0x2022,
    0x25D8, 0x25CB, 0x25D9, 0x2642, 0x2640, 0x266A, 0x266B, 0x263C,
    0x25BA, 0x25C4, 0x2195, 0x203C, 0x00B6, 0x00A7, 0x25AC, 0x21A8,
    0x2191, 0x2193, 0x2192, 0x2190, 0x221F, 0x2194, 0x25B2, 0x25BC,
};

static void vt_term_write(const char *buf, size_t n)
{
    manifold_write_to(COM_TERM_SCREEN, buf, n);
    char reply[256];
    size_t r;
    while ((r = com_read(COM_SOURCE_UART, reply, sizeof reply)) > 0)
        vt_reply(reply, r);
}

static void vt_term_init(void)
{
    sys_init();
    vga_set_framebuffer(vt_fb);
    for (int i = 0; i < 120; i++)
        vga_run_frame();
    /* The monitor's banner goes, and the run starts on a blank screen. */
    vt_term_write("\30\33c", 3);
    tb_screen_t v;
    tb_screen(&v);
    vt_rows = v.height;
    vt_cols = v.width;
}

static void vt_row(int y, char *out, size_t size)
{
    const mode0_cell_t *row = tb_screen_row((uint8_t)y);
    size_t o = 0, last = 0;
    for (int x = 0; x < vt_cols && o + 8 < size; x++)
    {
        const mode0_cell_t *c = &row[x];
        uint32_t cp = mode0_glyph_code(c->glyph);
        if (mode0_glyph_is_dec(c->glyph) && cp >= 0x5F && cp <= 0x7E)
            cp = vt_dec[cp - 0x5F];
        else if (cp < 0x20)
            cp = vt_cp437_low[cp]; /* a control drawn as its glyph */
        else if (cp == 0x7F)
            cp = 0x2302; /* DEL drawn: the house */
        else if (cp > 0x7F)
            cp = '?';
        o += vt_utf8(cp, out + o);
        if (cp != ' ')
            last = o;
    }
    out[last] = 0;
}

/* Finishes the lazy erases, and shows blinking text in its lit phase. */
static void vt_settle(void)
{
    for (int i = 0; i < TB_SCREEN_SETTLE; i++)
        cells_task();
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

/* A CRC-32 of the cells: glyphs, attributes and the colors they show. A
 * cell's color is a pixel or an entry of mode 0's palette; the pixel it is
 * drawn as is what goes in, so the golden screens hold whichever a cell
 * has. */
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
            /* The eight bytes golden.txt's digests were made from. */
            const struct
            {
                uint8_t font_code;
                uint8_t attributes;
                uint16_t fg_color;
                uint16_t bg_color;
                uint16_t ul_color;
            } shown = {mode0_glyph_code(row[x].glyph), vt_attributes(&row[x]),
                       mode0_color(colors[x].fg_color), mode0_color(colors[x].bg_color),
                       mode0_color(colors[x].ul_color)};
            crc = host_crc32(crc, &shown, sizeof shown);
        }
    }
    return crc;
}

/* A CRC-32 of what the screen shows: the cells, each line's size, and the
 * reverse screen. */
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
    return host_crc32(crc, &reverse, 1);
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

typedef struct
{
    char key[160];
    char id[24]; /* the title's number, "main", or m03 for the fourth menu met */
    int next;    /* the next choice to take, from 1 */
    int visits;
    uint32_t last; /* the cells of its last recording */
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
    /* Past what the console aims for: the VT102 and the xterm features a
     * program reaches for. */
    "VT320 features",
    "VT420 features",
    "VT520 features",
    /* Keeps the alternate-screen tests of 11.8.7 in color. */
    "Do not color after switching",
};

/* What to type where a test asks for more than Return, found by a prompt on
 * the screen; the first row that matches wins. A step is the keys typed at
 * one wait, as vt_type takes them, and locks are the keyboard's locks while
 * they are typed. A test that reads one key at a time and gives each a line gets
 * one key a step. Only the screen before the last step is recorded, which
 * shows what the steps before it did, and the last step is typed again at
 * every wait after it. */
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

static const vt_answer_t vt_answers[] = {
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
};

static const vt_answer_t *vt_answer; /* the row the walk is in */
static int vt_step;                  /* and the next step of it */

static pid_t vt_pid;
static int vt_slave = -1;

/* Whether vttest waits for a key: it is blocked reading its terminal, and
 * nothing is there for it to read. Linux shows the call a process is blocked
 * in, and vttest reads keys, and the terminal's replies, with read on fd 0. */
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
    return ok && nr == SYS_read && fd == 0 && !ioctl(vt_slave, FIONREAD, &queued) && !queued;
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
        struct pollfd p = {.fd = vt_master, .events = POLLIN};
        int k = poll(&p, 1, VT_POLL_MS);
        if (k < 0 && errno == EINTR)
            continue;
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
        if (quiet >= VT_STUCK_MS)
            return true;
    }
}

/* Waits for vttest to want a key, with the lazy erases finished so the cells
 * are what the screen shows. */
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
    {"kp-", HID_KEY_KEYPAD_SUBTRACT},
    {"kp,", HID_KEY_KEYPAD_COMMA},
    {"kp.", HID_KEY_KEYPAD_DECIMAL},
    {"kpenter", HID_KEY_KEYPAD_ENTER},
};

/* The key last pressed, which is the one that repeats. */
static uint8_t vt_held_usage, vt_held_flags;

/* Presses a key of a US keyboard, and what it sends goes to vttest as it
 * would from the console. */
static void vt_press(uint8_t usage, uint8_t flags)
{
    vt_held_usage = usage;
    vt_held_flags = flags;
    hostkey_press(usage, flags, true);
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

/* Types keys, a list of words: a key's name, "again" for the last key
 * repeating while it is held, or text, a character a key. ctrl- in front of
 * a name or a character holds Ctrl. locks are the keyboard's locks. */
static void vt_type(const char *keys, uint8_t locks)
{
    keyboard_set_locks(locks);
    while (*keys)
    {
        size_t len = strcspn(keys, " ");
        const char *word = keys;
        keys += len;
        keys += *keys == ' ';
        if (len == 5 && !memcmp(word, "again", 5))
        {
            if (vtkey_repeats())
                vt_press(vt_held_usage, vt_held_flags);
            continue;
        }
        uint8_t held = 0;
        if (len > 5 && !memcmp(word, "ctrl-", 5))
        {
            held |= VTKEY_CTRL;
            word += 5;
            len -= 5;
        }
        if (len == 3 && !memcmp(word, "kp", 2) && word[2] >= '0' && word[2] <= '9')
        {
            vt_press(word[2] == '0' ? HID_KEY_KEYPAD_0 : (uint8_t)(HID_KEY_KEYPAD_1 + word[2] - '1'),
                     held);
            continue;
        }
        size_t i = 0;
        while (i < sizeof vt_named / sizeof vt_named[0] &&
               (strlen(vt_named[i].name) != len || memcmp(word, vt_named[i].name, len)))
            i++;
        if (i < sizeof vt_named / sizeof vt_named[0])
        {
            vt_press(vt_named[i].usage, held);
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
            vt_press(usage, shift ? held | VTKEY_SHIFT : held);
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
    struct winsize ws = {.ws_row = (unsigned short)vt_rows, .ws_col = (unsigned short)vt_cols};
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
        const char *vttest = getenv("VTTEST");
        args[0] = (char *)(vttest ? vttest : "vttest");
        execvp(args[0], args);
        _exit(127);
    }

    vt_pid = pid;
    vt_slave = open(ptsname(vt_master), O_RDWR | O_NOCTTY);
    if (vt_slave < 0)
    {
        perror("vtrun: the pty's terminal");
        return 2;
    }
    int steps = 0;
    int status = 0;
    time_t start = time(NULL);
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
            while (m->next <= highest && vt_skipped(m->next))
                m->next++;
            int choice = m->next <= highest ? m->next++ : 0;
            char keys[24], what[64];
            snprintf(keys, sizeof keys, "%d return", choice);
            snprintf(what, sizeof what, "choice %d", choice);
            vt_send(name, keys, 0, what);
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
            if (last)
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
    if (vt_differ)
    {
        fprintf(stderr, "vtrun: %d screens differ from %s; they are saved in %s\n",
                vt_differ, getenv("VTGOLDEN"), vt_outdir);
        status = 1;
    }
    return status;
}
