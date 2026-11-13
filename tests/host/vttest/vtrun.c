/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Runs Thomas Dickey's vttest against a terminal, with nobody at the
 * keyboard. vttest runs in a pty. Everything it writes goes to the terminal,
 * and the terminal's replies go back to it, the way they would reach a host.
 * Every time vttest waits for a key, the screen is saved as text, and the
 * walk answers: the next choice on a menu, 0 on a menu with nothing left, q
 * where a test says so, and Return anywhere else.
 *
 * Built twice. vtrun drives the console terminal, term.c, inside the
 * emulator's core, and also saves each screen as a PNG. vtref drives
 * libvterm, a terminal that follows xterm closely, so the two runs can be
 * compared screen by screen.
 *
 * usage: vtrun OUTDIR [vttest arguments]
 *
 * A menu goes by the number in its title, as 11.1 for "Menu 11.1: ...", or
 * main, or m03 for the fourth menu met where it has no title. A screen is
 * named after the menu choice that led to it and its place after that
 * choice: 11.1_3-04.txt is the fourth screen of choice 3 of menu 11.1, and
 * 11.1-menu-02.txt is that menu the second time the walk is on it. Runs line up that way even where
 * one terminal takes a different number of screens. index.txt names each
 * menu and lists the screens in order with the key sent after each. */

#include <errno.h>
#include <poll.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define VT_FIRST_MS 7000 /* the longest vttest takes to start on a key: RIS sleeps 5 s */
#define VT_IDLE_MS 350   /* then quiet this long and it is waiting for one; vttest
                          * waits up to 300 ms for a reply */
#define VT_STEP_LIMIT 4000 /* screens before giving up on a walk gone wrong */
#define VT_TEST_LIMIT 60   /* screens one test may take before it is left */

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

#ifdef VT_REF

/* ------------------------------------------------------------------------
 * libvterm */

#include "vterm.h"

static VTerm *vt_term;
static VTermScreen *vt_screen;
static int vt_ref_rows = 30, vt_ref_cols = 80;

static void vt_output(const char *s, size_t len, void *user)
{
    (void)user;
    vt_reply(s, len);
}

static void vt_be_init(int *rows, int *cols)
{
    vt_term = vterm_new(vt_ref_rows, vt_ref_cols);
    vterm_set_utf8(vt_term, 0);
    vterm_output_set_callback(vt_term, vt_output, NULL);
    vt_screen = vterm_obtain_screen(vt_term);
    vterm_screen_reset(vt_screen, 1);
    *rows = vt_ref_rows;
    *cols = vt_ref_cols;
}

static void vt_be_feed(const char *buf, size_t n)
{
    vterm_input_write(vt_term, buf, n);
}

static void vt_be_row(int y, char *out, size_t size)
{
    size_t o = 0, last = 0;
    for (int x = 0; x < vt_ref_cols && o + 8 < size; x++)
    {
        VTermScreenCell cell;
        vterm_screen_get_cell(vt_screen, (VTermPos){.row = y, .col = x}, &cell);
        uint32_t cp = cell.chars[0];
        if (!cp)
            cp = ' ';
        if (cp >= 0x80 && cp < 0x2000 && cp != 0xB0 && cp != 0xB1 && cp != 0xA3 &&
            cp != 0xB7 && cp != 0x3C0)
            cp = '?';
        o += vt_utf8(cp, out + o);
        if (cp != ' ')
            last = o;
    }
    out[last] = 0;
}

static void vt_be_cursor(int *x, int *y, bool *visible)
{
    VTermPos pos;
    vterm_state_get_cursorpos(vterm_obtain_state(vt_term), &pos);
    *x = pos.col;
    *y = pos.row;
    *visible = true;
}

static void vt_be_settle(void)
{
}

static void vt_be_png(const char *path)
{
    (void)path;
}

#else

/* ------------------------------------------------------------------------
 * term.c in the emulator's core */

#include "core/com/com.h"
#include "core/com/manifold.h"
#include "core/sys/sys.h"
#include "core/term/term.h"
#include "core/vga/vga.h"
#include "core/vga/vga_emu.h"
#include "host/sokol/cli/png.h"

static uint32_t vt_fb[VGA_MAX_WIDTH * VGA_MAX_HEIGHT];

/* The DEC special graphics, 0x5F to 0x7E, as libvterm gives them. */
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

static void vt_be_feed(const char *buf, size_t n)
{
    manifold_write_to(COM_TERM_SCREEN, buf, n);
    char reply[256];
    size_t r;
    while ((r = manifold_uart_read(reply, sizeof reply)) > 0)
        vt_reply(reply, r);
}

static void vt_be_init(int *rows, int *cols)
{
    sys_init();
    vga_set_framebuffer(vt_fb);
    for (int i = 0; i < 120; i++)
        vga_run_frame();
    /* The monitor's banner goes, and the run starts on a blank screen. */
    vt_be_feed("\30\33c", 3);
    term_view_t v;
    term_view(&v);
    *rows = v.height;
    *cols = v.width;
}

static void vt_be_row(int y, char *out, size_t size)
{
    term_view_t v;
    term_view(&v);
    const term_data_t *row = term_view_row((uint8_t)y);
    size_t o = 0, last = 0;
    for (int x = 0; x < v.width && o + 8 < size; x++)
    {
        const term_data_t *c = &row[x];
        uint32_t cp = c->font_code;
        if ((c->attributes & TERM_ATTR_DEC) && cp >= 0x5F && cp <= 0x7E)
            cp = vt_dec[cp - 0x5F];
        else if (!cp)
            cp = ' ';
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

static void vt_be_cursor(int *x, int *y, bool *visible)
{
    term_view_t v;
    term_view(&v);
    *x = v.cursor_x; /* the width itself while a wrap is pending */
    *y = v.cursor_y;
    *visible = v.cursor_enabled;
}

/* Finishes the lazy erases, and shows blinking text in its lit phase. */
static void vt_be_settle(void)
{
    for (int i = 0; i < 4 * TERM_MAX_HEIGHT; i++)
        term_task();
    vga_run_frame();
    term_view_t v;
    for (int i = 0; i < 120; i++)
    {
        term_view(&v);
        if (!v.blink_phase)
            break;
        vga_run_frame();
    }
}

static void vt_be_png(const char *path)
{
    int w, h;
    vga_canvas_size(&w, &h);
    png_write(path, w, h, vt_fb);
}

#endif

/* ------------------------------------------------------------------------
 * The walk */

static int vt_rows, vt_cols;
static FILE *vt_index;
static const char *vt_outdir;
static char vt_path[64] = "start"; /* the choice that led here */
static unsigned vt_seq;              /* screens since that choice */

typedef struct
{
    char key[160];
    char id[24]; /* the title's number, "main", or m03 for the fourth menu met */
    int next;    /* the next choice to take, from 1 */
    int visits;
} vt_menu_t;

#define VT_MENUS 256
static vt_menu_t vt_menus[VT_MENUS];
static int vt_menu_count;

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

/* Reads until vttest has been quiet for VT_IDLE_MS, after waiting up to
 * VT_FIRST_MS for it to start answering the last key. False once it is
 * gone. */
static char vt_echo[64]; /* the keys last sent */

/* Whether buf is only the tty's echo of the keys last sent, which can arrive
 * while vttest sleeps and is not yet its answer. */
static bool vt_is_echo(const char *buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (buf[i] != '\r' && buf[i] != '\n' && !strchr(vt_echo, buf[i]))
            return false;
    return n <= strlen(vt_echo) + 2;
}

static bool vt_drain(void)
{
    int wait = VT_FIRST_MS;
    for (;;)
    {
        struct pollfd p = {.fd = vt_master, .events = POLLIN};
        int k = poll(&p, 1, wait);
        if (k == 0)
            return true;
        if (k < 0 && errno == EINTR)
            continue;
        char buf[4096];
        ssize_t n = read(vt_master, buf, sizeof buf);
        if (n <= 0)
            return false;
        vt_be_feed(buf, (size_t)n);
        if (wait == VT_FIRST_MS && vt_is_echo(buf, (size_t)n))
            continue;
        wait = VT_IDLE_MS;
    }
}

static bool vt_screen_has(const char *needle)
{
    char line[1024];
    for (int y = 0; y < vt_rows; y++)
    {
        vt_be_row(y, line, sizeof line);
        if (strstr(line, needle))
            return true;
    }
    return false;
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
    int cx, cy;
    bool visible;
    vt_be_cursor(&cx, &cy, &visible);
    for (int y = 0; y < vt_rows; y++)
    {
        vt_be_row(y, line, sizeof line);
        /* Only the prompt the cursor waits on: a test can leave an old
         * menu's prompt on the screen. */
        const char *p = strstr(line, "Enter choice number (0 - ");
        if (p && y == cy)
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
    fprintf(vt_index, "%s is the menu of \"%s\"\n", m->id, key);
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
        vt_be_row(y, line, sizeof line);
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

/* With VTGOLDEN set to a directory of approved screens, every text dump is
 * compared with the one of the same name there, and a difference fails the
 * run. */
static const char *vt_golden;
static int vt_differ;

static bool vt_same_file(const char *a, const char *b)
{
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    bool same = fa && fb;
    while (same)
    {
        int ca = fgetc(fa), cb = fgetc(fb);
        same = ca == cb;
        if (ca == EOF)
            break;
    }
    if (fa)
        fclose(fa);
    if (fb)
        fclose(fb);
    return same;
}

static void vt_compare(const char *path, const char *name)
{
    if (!vt_golden)
        return;
    char want[512];
    snprintf(want, sizeof want, "%s/%s.txt", vt_golden, name);
    if (!vt_same_file(path, want))
    {
        vt_differ++;
        fprintf(stderr, "vtrun: %s differs from %s\n", path, want);
    }
}

static void vt_shot(const char *name)
{
    vt_be_settle();
    char path[512];
    snprintf(path, sizeof path, "%s/%s.txt", vt_outdir, name);
    FILE *f = fopen(path, "w");
    if (f)
    {
        char line[1024];
        for (int y = 0; y < vt_rows; y++)
        {
            vt_be_row(y, line, sizeof line);
            fprintf(f, "%s\n", line);
        }
        int x, y;
        bool visible;
        vt_be_cursor(&x, &y, &visible);
        fprintf(f, "-- cursor %d,%d%s\n", x, y, visible ? "" : " hidden");
        fclose(f);
        vt_compare(path, name);
    }
    snprintf(path, sizeof path, "%s/%s.png", vt_outdir, name);
    vt_be_png(path);
}

static void vt_send(const char *name, const char *keys, const char *what)
{
    fprintf(vt_index, "%s %s\n", name, what);
    fflush(vt_index);
    snprintf(vt_echo, sizeof vt_echo, "%s", keys);
    vt_reply(keys, strlen(keys));
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s OUTDIR [vttest arguments]\n", argv[0]);
        return 2;
    }
    vt_outdir = argv[1];
    vt_golden = getenv("VTGOLDEN");
    char path[512];
    snprintf(path, sizeof path, "%s/index.txt", vt_outdir);
    vt_index = fopen(path, "w");
    if (!vt_index)
    {
        fprintf(stderr, "vtrun: cannot write %s\n", path);
        return 2;
    }

    vt_be_init(&vt_rows, &vt_cols);
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

    int steps = 0;
    int status = 0;
    while (steps++ < VT_STEP_LIMIT && vt_drain())
    {
        char name[96], key[160], number[24];
        int highest = 0;
        if (vt_menu_on_screen(key, sizeof key, number, sizeof number, &highest))
        {
            vt_menu_t *m = vt_menu(key, number);
            if (!m)
            {
                fprintf(vt_index, "too many menus\n");
                status = 1;
                break;
            }
            const char *id = m->id;
            snprintf(name, sizeof name, "%s-menu-%02d", id, ++m->visits);
            vt_shot(name);
            while (m->next <= highest && vt_skipped(m->next))
                m->next++;
            int choice = m->next <= highest ? m->next++ : 0;
            char keys[16], what[64];
            snprintf(keys, sizeof keys, "%d\r", choice);
            snprintf(what, sizeof what, "choice %d", choice);
            vt_send(name, keys, what);
            snprintf(vt_path, sizeof vt_path, "%s_%d", id, choice);
            vt_seq = 0;
            if (!strcmp(id, "main") && choice == 0)
                break;
        }
        else
        {
            if (vt_seq == VT_TEST_LIMIT)
            {
                fprintf(vt_index, "%s never came back to its menu\n", vt_path);
                status = 1;
                break;
            }
            snprintf(name, sizeof name, "%s-%02u", vt_path, ++vt_seq);
            vt_shot(name);
            /* "Push <RETURN>" where the cursor waits wins over a prompt left
             * higher on the screen. */
            const char *keys = "\r";
            int cx, cy;
            bool visible;
            char row[1024];
            vt_be_cursor(&cx, &cy, &visible);
            vt_be_row(cy, row, sizeof row);
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
        fprintf(vt_index, "step limit\n");
        status = 1;
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    fclose(vt_index);
    if (vt_differ)
    {
        fprintf(stderr, "vtrun: %d screens differ from %s\n", vt_differ, vt_golden);
        status = 1;
    }
    return status;
}
