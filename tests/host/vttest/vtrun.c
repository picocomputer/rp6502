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
 * q where a test says so, and Return anywhere else.
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
 * walk, with the key sent after each screen. With VTGOLDEN naming an
 * approved screens.txt, the run fails when a screen differs, is new, or is
 * gone, and only those screens are saved as text and PNG for a person to look
 * at; without it, every screen is. */

#include "core/com/com.h"
#include "core/com/manifold.h"
#include "core/sys/sys.h"
#include "core/term/cells.h"
#include "core/vga/vga.h"
#include "core/vga/vga_emu.h"
#include "host/host.h"
#include "host/sokol/cli/png.h"
#include <errno.h>
#include <poll.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define VT_FIRST_MS 7000 /* the longest vttest takes to start on a key: RIS sleeps 5 s */
#define VT_IDLE_MS 350   /* then quiet this long and it is waiting for one */
#define VT_RAW_MS 1000   /* the same in raw mode, where vttest asks the terminal
                          * for reports and pauses 100 to 300 ms around them,
                          * longer on a busy machine */
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
    while ((r = manifold_uart_read(reply, sizeof reply)) > 0)
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
    cells_view_t v;
    cells_view(&v);
    vt_rows = v.height;
    vt_cols = v.width;
}

static void vt_row(int y, char *out, size_t size)
{
    const term_data_t *row = cells_view_row((uint8_t)y);
    size_t o = 0, last = 0;
    for (int x = 0; x < vt_cols && o + 8 < size; x++)
    {
        const term_data_t *c = &row[x];
        uint32_t cp = c->font_code;
        if ((c->attributes & TERM_ATTR_DEC) && cp >= 0x5F && cp <= 0x7E)
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
    for (int i = 0; i < 4 * TERM_MAX_HEIGHT; i++)
        cells_task();
    vga_run_frame();
    cells_view_t v;
    for (int i = 0; i < 120; i++)
    {
        cells_view(&v);
        if (!v.blink_phase)
            break;
        vga_run_frame();
    }
}

/* A CRC-32 of the cells: glyphs, attributes and colors. */
static uint32_t vt_cells(void)
{
    uint32_t crc = 0;
    for (int y = 0; y < vt_rows; y++)
        crc = host_crc32(crc, cells_view_row((uint8_t)y), (size_t)vt_cols * sizeof(term_data_t));
    return crc;
}

/* A CRC-32 of what the screen shows: the cells, each line's size, and the
 * reverse screen. */
static uint32_t vt_digest(void)
{
    uint32_t crc = vt_cells();
    for (int y = 0; y < vt_rows; y++)
    {
        uint8_t size = cells_view_row_size((uint8_t)y);
        crc = host_crc32(crc, &size, 1);
    }
    cells_view_t v;
    cells_view(&v);
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
    cells_view_t v;
    cells_view(&v);
    vt_last = vt_cells();
    vt_fed = 0;
    char line[160];
    snprintf(line, sizeof line, "%s %08X cursor %d,%d style %d%s", name, vt_digest(),
             v.cursor_x, v.cursor_y, v.cursor_style, v.cursor_enabled ? "" : " hidden");
    fprintf(vt_screens, "%s\n", line);
    fflush(vt_screens);
    bool ok = !vt_have_golden || vt_approved(name, line);
    if (!ok)
        vt_differ++;
    if (!vt_have_golden || !ok)
        vt_save(name);
}

static char vt_echo[64]; /* the keys last sent */

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
    if (vt_fed <= strlen(vt_echo) + 2)
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
    "Test of keyboard",       /* a person presses keys */
    "Modify test-parameters", /* changes the rest of the run */
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

/* What to type where a test asks for more than Return. */
static const struct
{
    const char *prompt;
    const char *keys;
} vt_answers[] = {
    {"Press 'q' to quit", "q"},
    {"Please enter the", "vttest\r"}, /* a window title, a font name */
};

/* Whether buf is only the tty's echo of the keys last sent, which can arrive
 * while vttest sleeps and is not yet its answer. */
static bool vt_is_echo(const char *buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (buf[i] != '\r' && buf[i] != '\n' && !strchr(vt_echo, buf[i]))
            return false;
    return n <= strlen(vt_echo) + 2;
}

/* Whether vttest has the tty in raw mode. It does that for its exchanges
 * with the terminal, though some of its tests also wait for a key that way. */
static bool vt_raw(void)
{
    struct termios t;
    return !tcgetattr(vt_master, &t) && !(t.c_lflag & ICANON);
}

/* Reads until vttest has been quiet for VT_IDLE_MS, or VT_RAW_MS with the
 * tty in raw mode, after waiting up to `first` ms for it to start. False
 * once it is gone. */
static bool vt_drain(int first)
{
    int wait = first;
    for (;;)
    {
        struct pollfd p = {.fd = vt_master, .events = POLLIN};
        int k = poll(&p, 1, wait);
        if (k == 0)
        {
            if (wait == VT_IDLE_MS && vt_raw())
            {
                wait = VT_RAW_MS - VT_IDLE_MS;
                continue;
            }
            return true;
        }
        if (k < 0 && errno == EINTR)
            continue;
        char buf[4096];
        ssize_t n = read(vt_master, buf, sizeof buf);
        if (n <= 0)
            return false;
        vt_feed(buf, (size_t)n);
        if (wait == first && vt_is_echo(buf, (size_t)n))
            continue;
        wait = VT_IDLE_MS;
    }
}

/* Waits for vttest to want a key, with the lazy erases finished so the cells
 * are what the screen shows. A blank screen is vttest in a pause, such as the
 * five seconds it sleeps after a reset, so it is given that long to draw
 * something before the walk presses a key into its pause. */
static bool vt_wait(void)
{
    if (!vt_drain(VT_FIRST_MS))
        return false;
    vt_settle();
    if (vt_blank())
    {
        if (!vt_drain(VT_FIRST_MS))
            return false;
        vt_settle();
    }
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
    cells_view_t v;
    cells_view(&v);
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

static void vt_send(const char *name, const char *keys, const char *what)
{
    vt_log("%s %s%s\n", name, what, vt_132 ? " (132 columns)" : "");
    snprintf(vt_echo, sizeof vt_echo, "%s", keys);
    vt_reply(keys, strlen(keys));
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
        /* A new pty runs at 38400 baud on Linux and 9600 on macOS, and
         * vttest's main menu shows the speed when it is not 9600. */
        struct termios t;
        if (!tcgetattr(0, &t))
        {
            cfsetispeed(&t, B38400);
            cfsetospeed(&t, B38400);
            tcsetattr(0, TCSANOW, &t);
        }
        setenv("TERM", "vt102", 1);
        const char *vttest = getenv("VTTEST");
        args[0] = (char *)(vttest ? vttest : "vttest");
        execvp(args[0], args);
        _exit(127);
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
            char keys[16], what[64];
            snprintf(keys, sizeof keys, "%d\r", choice);
            snprintf(what, sizeof what, "choice %d", choice);
            vt_send(name, keys, what);
            snprintf(vt_path, sizeof vt_path, "%s_%d", m->id, choice);
            vt_seq = 0;
            vt_left = m;
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
            snprintf(name, sizeof name, "%s-%02u", vt_path, ++vt_seq);
            vt_shot(name);
            /* "Push <RETURN>" where the cursor waits wins over a prompt left
             * higher on the screen. */
            const char *keys = "\r";
            cells_view_t v;
            cells_view(&v);
            char row[1024];
            vt_row(v.cursor_y, row, sizeof row);
            for (size_t i = 0; !strstr(row, "Push <RETURN>") &&
                               i < sizeof vt_answers / sizeof vt_answers[0]; i++)
                if (vt_screen_has(vt_answers[i].prompt))
                {
                    keys = vt_answers[i].keys;
                    break;
                }
            vt_send(name, keys, keys[0] == '\r' ? "return" : keys);
        }
    }
    if (steps > VT_STEP_LIMIT)
    {
        vt_log("step limit\n");
        status = 1;
    }
    /* macOS keeps an exiting process until the output it left on its tty
     * has been read, so the pty is closed before waiting for vttest. */
    kill(pid, SIGKILL);
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
