/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "xmirror.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define XM_ANSWER_MS 10000 /* for xterm to start, or to print a screen */

static pid_t xm_xvfb = -1, xm_xterm = -1;
static int xm_tty = -1; /* the pseudoterminal's side a program has */
static int xm_rows, xm_cols;
static char xm_dump[512];

/* What was written and not yet passed on: an escape sequence not finished,
 * held so a screen is never asked for in the middle of one. */
static char xm_held[4096];
static size_t xm_held_len;
enum
{
    XM_GROUND,
    XM_ESC,    /* after ESC */
    XM_CSI,    /* in CSI's parameters and intermediates */
    XM_STRING, /* in OSC, DCS, APC, PM or SOS */
    XM_STRING_ESC,
};
static int xm_state = XM_GROUND;

/* Whether xterm is a VT52 now: CSI ? 2 l makes it one, and ESC < or a
 * reset (ESC c) ends it. */
static bool xm_vt52;
static char xm_tail[6];

static long xm_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static void xm_put(const char *buf, size_t n)
{
    while (n)
    {
        ssize_t w = write(xm_tty, buf, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0 && errno == EAGAIN)
        {
            /* xterm is behind; what it has to say is read meanwhile so it
             * never waits on its own replies. */
            char sink[4096];
            while (read(xm_tty, sink, sizeof sink) > 0)
                ;
            struct pollfd p = {.fd = xm_tty, .events = POLLOUT};
            poll(&p, 1, 10);
            continue;
        }
        if (w <= 0)
            return;
        buf += w;
        n -= (size_t)w;
    }
}

/* Reads what xterm sent, keeping the tail of it in buf. */
static size_t xm_read(char *buf, size_t len, size_t size, int ms)
{
    struct pollfd p = {.fd = xm_tty, .events = POLLIN};
    if (poll(&p, 1, ms) <= 0)
        return len;
    char tmp[4096];
    ssize_t n = read(xm_tty, tmp, sizeof tmp);
    if (n <= 0)
        return len;
    if ((size_t)n >= size)
    {
        memcpy(buf, tmp + n - (size - 1), size - 1);
        len = size - 1;
    }
    else
    {
        if (len + (size_t)n >= size)
        {
            size_t drop = len + (size_t)n - (size - 1);
            memmove(buf, buf + drop, len - drop);
            len -= drop;
        }
        memcpy(buf + len, tmp, (size_t)n);
        len += (size_t)n;
    }
    buf[len] = 0;
    return len;
}

bool xm_start(int rows, int cols, const char *dir)
{
    if (rows > XM_MAX_ROWS || cols > XM_MAX_COLS)
        return false;
    xm_rows = rows;
    xm_cols = cols;
    snprintf(xm_dump, sizeof xm_dump, "%s/xterm.print", dir);

    /* Xvfb on a display of its own, the first free one from :50. It
     * listens only on Linux's abstract sockets, as a WSL machine's
     * /tmp/.X11-unix is not its to use. It writes the display's number to
     * the pipe once it is up, or exits. */
    char display[32] = "";
    for (int number = 50; number < 100 && !display[0]; number++)
    {
        int dfd[2];
        if (pipe(dfd))
            return false;
        xm_xvfb = fork();
        if (xm_xvfb == 0)
        {
            close(dfd[0]);
            int null = open("/dev/null", O_WRONLY);
            dup2(null, 1);
            dup2(null, 2);
            char fd[16], name[16];
            snprintf(fd, sizeof fd, "%d", dfd[1]);
            snprintf(name, sizeof name, ":%d", number);
            execlp("Xvfb", "Xvfb", name, "-displayfd", fd, "-nolisten", "tcp", "-nolisten",
                   "unix", "-screen", "0", "1600x1200x24", (char *)NULL);
            _exit(127);
        }
        close(dfd[1]);
        char got[16];
        size_t gl = 0;
        long until = xm_ms() + XM_ANSWER_MS;
        while (xm_ms() < until && gl < sizeof got - 1)
        {
            struct pollfd p = {.fd = dfd[0], .events = POLLIN};
            if (poll(&p, 1, 100) <= 0)
                continue;
            char c;
            if (read(dfd[0], &c, 1) != 1 || c == '\n')
                break; /* an end with nothing is Xvfb gone */
            got[gl++] = c;
        }
        close(dfd[0]);
        if (gl)
            snprintf(display, sizeof display, ":%d", number);
        else
        {
            kill(xm_xvfb, SIGTERM);
            waitpid(xm_xvfb, NULL, 0);
            xm_xvfb = -1;
        }
    }
    if (!display[0])
    {
        fprintf(stderr, "xmirror: Xvfb did not start\n");
        return false;
    }

    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) || unlockpt(master))
        return false;
    const char *name = ptsname(master);
    xm_tty = open(name, O_RDWR | O_NOCTTY);
    if (xm_tty < 0)
        return false;
    struct termios tio;
    tcgetattr(xm_tty, &tio);
    cfmakeraw(&tio);
    tcsetattr(xm_tty, TCSANOW, &tio);

    char sopt[64], geometry[32], printer[600];
    const char *slash = strrchr(name, '/');
    snprintf(sopt, sizeof sopt, "-S%s/%d", slash ? slash + 1 : name, master);
    snprintf(geometry, sizeof geometry, "%dx%d", cols, rows);
    snprintf(printer, sizeof printer, "*printerCommand: cat > '%s'", xm_dump);
    xm_xterm = fork();
    if (xm_xterm == 0)
    {
        int null = open("/dev/null", O_WRONLY);
        dup2(null, 1);
        dup2(null, 2);
        setenv("DISPLAY", display, 1);
        setenv("LC_ALL", "C", 1);
        /* 8-bit, as the console is: no UTF-8, and the 8-bit codes as
         * Latin-1. The screen stays 80 columns wide when asked for 132, as
         * the console's does. The print has each cell's attributes and
         * colors, and a line a row. */
        execlp("xterm", "xterm", sopt, "+u8", "-geometry", geometry,
               "-xrm", "*locale: false", "-xrm", "*c132: false",
               "-xrm", "*saveLines: 0", "-xrm", printer,
               "-xrm", "*printAttributes: 2", "-xrm", "*printerAutoClose: true",
               "-xrm", "*printerFormFeed: false", "-xrm", "*printerNewLine: true",
               (char *)NULL);
        _exit(127);
    }
    close(master);
    fcntl(xm_tty, F_SETFL, fcntl(xm_tty, F_GETFL) | O_NONBLOCK);

    /* xterm says its window's ID, a line, when it is up. */
    char buf[256];
    size_t len = 0;
    buf[0] = 0;
    long until = xm_ms() + XM_ANSWER_MS;
    while (xm_ms() < until && !strchr(buf, '\n'))
        len = xm_read(buf, len, sizeof buf, 100);
    if (!strchr(buf, '\n'))
    {
        fprintf(stderr, "xmirror: xterm did not start\n");
        xm_stop();
        return false;
    }
    return true;
}

void xm_write(const char *buf, size_t n)
{
    if (xm_tty < 0)
        return;
    for (size_t i = 0; i < n; i++)
    {
        const uint8_t c = (uint8_t)buf[i];
        memmove(xm_tail, xm_tail + 1, sizeof xm_tail - 1);
        xm_tail[sizeof xm_tail - 1] = (char)c;
        if (!xm_vt52 && !memcmp(xm_tail + 1, "\33[?2l", 5))
            xm_vt52 = true;
        else if (xm_vt52 && (!memcmp(xm_tail + 4, "\33<", 2) || !memcmp(xm_tail + 4, "\33c", 2)))
            xm_vt52 = false;
        if (xm_held_len < sizeof xm_held)
            xm_held[xm_held_len++] = (char)c;
        switch (xm_state)
        {
        case XM_GROUND:
            if (c == 0x1B)
                xm_state = XM_ESC;
            break;
        case XM_ESC:
            if (c == '[')
                xm_state = XM_CSI;
            else if (c == ']' || c == 'P' || c == '_' || c == '^' || c == 'X')
                xm_state = XM_STRING;
            else if (c >= 0x30 && c <= 0x7E)
                xm_state = XM_GROUND;
            else if (c == 0x18 || c == 0x1A)
                xm_state = XM_GROUND; /* CAN and SUB cancel */
            break;
        case XM_CSI:
            if ((c >= 0x40 && c <= 0x7E) || c == 0x18 || c == 0x1A)
                xm_state = XM_GROUND;
            else if (c == 0x1B)
                xm_state = XM_ESC;
            break;
        case XM_STRING:
            if (c == 0x07 || c == 0x18 || c == 0x1A)
                xm_state = XM_GROUND;
            else if (c == 0x1B)
                xm_state = XM_STRING_ESC;
            break;
        case XM_STRING_ESC:
            xm_state = c == '\\' ? XM_GROUND : c == 0x1B ? XM_STRING_ESC : XM_STRING;
            break;
        }
        if (xm_state == XM_GROUND)
        {
            /* The media copies (MC, CSI i and CSI ? i) are not passed on:
             * the console has no printer, and xterm's is the mirror's. */
            const bool mc = xm_held_len > 2 && xm_held[0] == 0x1B && xm_held[1] == '[' &&
                            c == 'i';
            if (!mc)
                xm_put(xm_held, xm_held_len);
            xm_held_len = 0;
        }
    }
}

/* ------------------------------------------------------------------------
 * The print */

typedef struct
{
    int fg, bg; /* -1 for the default */
    uint32_t fg_rgb, bg_rgb;
    bool bold, faint, inverse, conceal;
    uint8_t attr;
} xm_pen_t;

static void xm_sgr(xm_pen_t *p, const char *params)
{
    /* The parameters as numbers, ':' and ';' alike. */
    int v[32], n = 0;
    const char *s = params;
    while (n < 32)
    {
        v[n++] = atoi(s);
        while (*s >= '0' && *s <= '9')
            s++;
        if (*s != ';' && *s != ':')
            break;
        s++;
    }
    for (int i = 0; i < n; i++)
    {
        const int c = v[i];
        if (c == 0)
            *p = (xm_pen_t){.fg = -1, .bg = -1};
        else if (c == 1)
            p->bold = true;
        else if (c == 2)
            p->faint = true;
        else if (c == 3)
            p->attr |= XM_ATTR_ITALIC;
        else if (c == 4)
            p->attr |= XM_ATTR_UL;
        else if (c == 5 || c == 6)
            p->attr |= XM_ATTR_BLINK;
        else if (c == 7)
            p->inverse = true;
        else if (c == 8)
            p->conceal = true;
        else if (c == 9)
            p->attr |= XM_ATTR_STRIKE;
        else if (c == 21)
            p->attr |= XM_ATTR_UL | XM_ATTR_DUL;
        else if (c == 53)
            p->attr |= XM_ATTR_OVERLINE;
        else if (c >= 30 && c <= 37)
            p->fg = c - 30;
        else if (c >= 40 && c <= 47)
            p->bg = c - 40;
        else if (c >= 90 && c <= 97)
            p->fg = c - 90 + 8;
        else if (c >= 100 && c <= 107)
            p->bg = c - 100 + 8;
        else if (c == 39)
            p->fg = -1;
        else if (c == 49)
            p->bg = -1;
        else if ((c == 38 || c == 48) && i + 2 < n && v[i + 1] == 5)
        {
            *(c == 38 ? &p->fg : &p->bg) = v[i + 2];
            i += 2;
        }
        else if ((c == 38 || c == 48) && i + 4 < n && v[i + 1] == 2)
        {
            /* 38:2::r:g:b has an empty color space before the three */
            int at = i + 5 < n && strstr(params, ":2::") ? i + 3 : i + 2;
            uint32_t rgb = XM_RGB5(v[at] >> 3, v[at + 1] >> 3, v[at + 2] >> 3);
            if (c == 38)
                p->fg = 512, p->fg_rgb = rgb;
            else
                p->bg = 512, p->bg_rgb = rgb;
            i = at + 2;
        }
    }
}

/* The colors a cell of this pen shows, as the console works them out: bold
 * brightens the first eight and the default foreground, which becomes
 * entry 15; faint dims the ink; reverse swaps; concealed ink is the
 * background. */
static void xm_colors(const xm_pen_t *p, xm_cell_t *c)
{
    uint32_t fg = p->fg == 512 ? p->fg_rgb : p->fg < 0 ? XM_DEFAULT_FG : (uint32_t)p->fg;
    uint32_t bg = p->bg == 512 ? p->bg_rgb : p->bg < 0 ? XM_DEFAULT_BG : (uint32_t)p->bg;
    if (p->bold && !p->conceal)
    {
        if (fg < 8)
            fg += 8;
        else if (fg == XM_DEFAULT_FG)
            fg = 15;
    }
    if (p->faint && !(fg & XM_RGB))
        fg |= XM_FAINT;
    if (p->inverse)
    {
        uint32_t t = fg;
        fg = bg;
        bg = t;
    }
    if (p->conceal)
        fg = bg;
    c->fg = fg;
    c->bg = bg;
}

/* The DEC special graphics, 0x5F to 0x7E, as Unicode. */
static const uint32_t xm_dec[32] = {
    ' ', 0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0,
    0x00B1, 0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C,
    0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534,
    0x252C, 0x2502, 0x2A7D, 0x2A7E, 0x03C0, 0x2260, 0x00A3, 0x00B7,
};

static void xm_parse(const unsigned char *d, size_t n, xm_screen_t *s)
{
    xm_pen_t pen = {.fg = -1, .bg = -1};
    int y = 0, x = 0;
    bool graphics = false; /* SO: xterm prints the line drawing in G1 */
    bool fresh = false;    /* an SGR came right before this cell */
    for (int r = 0; r < XM_MAX_ROWS; r++)
    {
        s->line[r] = 5;
        for (int c = 0; c < XM_MAX_COLS; c++)
            s->cell[r][c] = (xm_cell_t){.cp = ' ', .fg = XM_DEFAULT_FG, .bg = XM_DEFAULT_BG};
    }
    size_t i = 0;
    while (i < n && y < s->rows)
    {
        unsigned char c = d[i++];
        if (c == 0x1B && i < n && d[i] == '[')
        {
            size_t j = ++i;
            while (j < n && (d[j] < 0x40 || d[j] > 0x7E))
                j++;
            if (j < n && d[j] == 'm')
            {
                char params[128];
                size_t len = j - i < sizeof params - 1 ? j - i : sizeof params - 1;
                memcpy(params, d + i, len);
                params[len] = 0;
                xm_sgr(&pen, params);
                fresh = true;
            }
            i = j + 1;
        }
        else if (c == 0x1B && i < n && d[i] == '#')
        {
            if (i + 1 < n)
                s->line[y] = (uint8_t)(d[i + 1] - '0');
            i += 2;
        }
        else if (c == 0x1B)
            i++;
        else if (c == '\r')
            x = 0;
        else if (c == '\n')
        {
            y++;
            x = 0;
            graphics = false;
        }
        else if (c == 0x0E)
            graphics = true;
        else if (c == 0x0F)
            graphics = false;
        else if (c >= 0x20)
        {
            uint32_t cp = c;
            if (c >= 0xC0 && i < n)
            {
                /* UTF-8, which the print is in past ASCII */
                int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
                cp = c & (0x3F >> more);
                for (int k = 0; k < more && i < n; k++)
                    cp = (cp << 6) | (d[i++] & 0x3F);
            }
            else if (graphics && c >= 0x5F && c <= 0x7E)
                cp = xm_dec[c - 0x5F];
            /* xterm prints an SGR before each cell that has attributes or
             * colors, and nothing before one that has none, so a cell with
             * no SGR before it has none. Except a blank: xterm prints a cell
             * never written, or erased, as a space with no SGR whatever its
             * colors, so of a blank with no SGR before it only the text is
             * known. */
            if (!fresh)
                pen = (xm_pen_t){.fg = -1, .bg = -1};
            if (x < XM_MAX_COLS)
            {
                xm_cell_t *cell = &s->cell[y][x];
                cell->cp = cp;
                cell->attr = pen.attr;
                cell->known = fresh || cp != ' ';
                xm_colors(&pen, cell);
            }
            fresh = false;
            x++;
        }
    }
}

static bool xm_reply_number(const char *buf, const char *lead, char final, int *a, int *b)
{
    /* the last reply of this kind in buf */
    const char *found = NULL;
    for (const char *p = buf; (p = strstr(p, lead)); p++)
    {
        const char *q = p + strlen(lead);
        int x = 0, y = 0;
        char f = 0;
        if (sscanf(q, "%d;%d%c", &x, &y, &f) == 3 && f == final)
        {
            *a = x;
            *b = y;
            found = p;
        }
    }
    return found != NULL;
}

/* Whether DECRQM's last answer in buf has private mode n set. */
static bool xm_mode_set(const char *buf, int n)
{
    char lead[16];
    snprintf(lead, sizeof lead, "\33[?%d;", n);
    const char *last = NULL;
    for (const char *p = buf; (p = strstr(p, lead)); p++)
        last = p;
    return last && last[strlen(lead)] == '1';
}

bool xm_snap(xm_screen_t *s)
{
    if (xm_tty < 0)
        return false;
    unlink(xm_dump);
    /* Print the screen, all of it and not only the scrolling region
     * (DECPEX), then the cursor, then the reverse screen, then a mode
     * nobody asks about, whose answer says all of it is done. */
    static const char ask[] = "\33[?19h\33[i\33[6n\33[?5$p\33[?6$p\33[?69$p"
                              "\33P$qr\33\\\33P$qs\33\\\33[?31337$p";
    /* A VT52 knows none of that: it goes back to being one after. */
    if (xm_vt52)
        xm_put("\33<", 2);
    xm_put(ask, sizeof ask - 1);
    if (xm_vt52)
        xm_put("\33[?2l", 5);
    char buf[8192];
    size_t len = 0;
    buf[0] = 0;
    long until = xm_ms() + XM_ANSWER_MS;
    while (!strstr(buf, "\33[?31337;") && xm_ms() < until)
        len = xm_read(buf, len, sizeof buf, 100);
    if (!strstr(buf, "\33[?31337;"))
    {
        fprintf(stderr, "xmirror: xterm did not answer\n");
        return false;
    }
    s->rows = xm_rows;
    s->cols = xm_cols;
    /* The cursor's report is from the margins' corner in origin mode
     * (DECOM), the left margin's only with DECLRMM set. */
    int row = 1, col = 1, top = 1, bottom = 0, left = 1, right = 0;
    xm_reply_number(buf, "\33[", 'R', &row, &col);
    xm_reply_number(buf, "\33P1$r", 'r', &top, &bottom);
    xm_reply_number(buf, "\33P1$r", 's', &left, &right);
    if (xm_mode_set(buf, 6))
    {
        row += top - 1;
        if (xm_mode_set(buf, 69))
            col += left - 1;
    }
    s->cursor_y = row - 1;
    s->cursor_x = col - 1;
    s->reverse = xm_mode_set(buf, 5);

    /* The print is written by a cat of xterm's, which may finish after
     * xterm has answered. It is whole with a line for every row. */
    static unsigned char d[1 << 16];
    size_t n = 0;
    for (until = xm_ms() + XM_ANSWER_MS; xm_ms() < until;)
    {
        FILE *f = fopen(xm_dump, "rb");
        n = 0;
        if (f)
        {
            n = fread(d, 1, sizeof d, f);
            fclose(f);
        }
        int lines = 0;
        for (size_t i = 0; i < n; i++)
            lines += d[i] == '\n';
        if (lines >= xm_rows)
        {
            xm_parse(d, n, s);
            return true;
        }
        struct timespec nap = {0, 200000};
        nanosleep(&nap, NULL);
    }
    fprintf(stderr, "xmirror: no print from xterm\n");
    return false;
}

void xm_stop(void)
{
    if (xm_xterm > 0)
    {
        kill(xm_xterm, SIGTERM);
        waitpid(xm_xterm, NULL, 0);
        xm_xterm = -1;
    }
    if (xm_xvfb > 0)
    {
        kill(xm_xvfb, SIGTERM);
        waitpid(xm_xvfb, NULL, 0);
        xm_xvfb = -1;
    }
    if (xm_tty >= 0)
    {
        close(xm_tty);
        xm_tty = -1;
    }
}
