/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "host/sokol/scr.h"
#include "host/sokol/png.h"
#include "core/sys/proc.h"
#include "core/sys/kbd.h"
#include "core/hid/mou.h"
#include "host/sokol/pad.h"
#include "core/hid/tab.h"
#include "core/com/com.h"
#include "core/wdc/cpu.h"
#include "core/mem/mem.h"
#include "core/vga/vga_emu.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define SCR_LINE_MAX 1024
#define SCR_CAP_SIZE 65536
#define SCR_CAP_KEEP 4096   /* console tail kept when the capture fills */
#define SCR_WAIT_FRAMES 600 /* default budget for a command that blocks */

/* What the script is waiting for before its next command runs. */
typedef enum
{
    SCR_IDLE,
    SCR_FRAMES,
    SCR_TEXT,
    SCR_BYTE,
    SCR_TYPING,
    SCR_EXIT,
} scr_wait_t;

static FILE *scr_file;
static const char *scr_path = "<script>"; /* named before a file, for scr_command */
static int scr_line_no;
static bool scr_run, scr_fail;

static scr_wait_t scr_wait;
/* Frames still owed to the pending wait. scr_task hands control back only when
 * one is due, so `run 600` costs exactly 600 frames and a budget expires on the
 * frame it names — neither depends on how often anything else polls. */
static unsigned long scr_budget;
static char scr_needle[256];
static int scr_exit_want;

/* Where a wait on memory is looking. The byte is read at the frame boundary,
 * which is why the program on the other end holds its answer until the script
 * acknowledges it rather than publishing it for a cycle and moving on. */
static uint8_t *scr_addr_base;
static long scr_addr;
static uint8_t scr_addr_want;

/* The canvas as `mark` last saw it. A test of a graphic is usually "this moved"
 * or "this held still", which a remembered hash answers without a literal one
 * that every unrelated rendering change would invalidate. */
static uint32_t scr_mark;
static bool scr_marked;

/* Every player's report, assembled here and handed to pad_host_report — the
 * same shape the web and Android hosts keep, so a scripted pad reaches XRAM
 * through the code a real one does. */
static struct
{
    bool connected;
    bool sticks;
    uint8_t type;
    uint8_t dpad, button0, button1;
    int lx, ly, rx, ry, lt, rt;
} scr_pad[4];

/* ------------------------------------------------------------------ */
/* Console capture                                                     */
/* ------------------------------------------------------------------ */

static char scr_cap[SCR_CAP_SIZE];
static size_t scr_cap_len;

static void scr_tap(const char *buf, int len)
{
    for (int i = 0; i < len; i++)
    {
        if (scr_cap_len == sizeof scr_cap - 1)
        {
            /* Only output nothing has matched yet gets this far; keep the tail
             * so a needle straddling the discard still has somewhere to land. */
            memmove(scr_cap, scr_cap + scr_cap_len - SCR_CAP_KEEP, SCR_CAP_KEEP);
            scr_cap_len = SCR_CAP_KEEP;
        }
        scr_cap[scr_cap_len++] = buf[i];
    }
    scr_cap[scr_cap_len] = 0;
}

/* Take the console up to and including a match, leaving the rest to be matched
 * by whatever the script checks next — two needles in one burst of output are
 * two checks, not a race. */
static void scr_cap_take(const char *through)
{
    size_t used = (size_t)(through - scr_cap);
    memmove(scr_cap, scr_cap + used, scr_cap_len - used + 1);
    scr_cap_len -= used;
}

/* ------------------------------------------------------------------ */
/* Parsing                                                             */
/* ------------------------------------------------------------------ */

/* What the console actually held. The capture is the wire: it carries the line
 * editor's escapes and the echo of anything the script typed, so a needle that
 * spans a cursor move never matches and the only way to see why is to look. */
static void scr_show_capture(void)
{
    size_t from = scr_cap_len > 200 ? scr_cap_len - 200 : 0;
    fputs("  console: ", stderr);
    for (size_t i = from; i < scr_cap_len; i++)
    {
        unsigned char c = (unsigned char)scr_cap[i];
        if (c >= ' ' && c < 0x7F)
            fputc(c, stderr);
        else
            fprintf(stderr, "\\x%02X", c);
    }
    fputc('\n', stderr);
}

/* One line per command once `reply` has turned them on: ok, ok <values>, or
 * fail <why>. Off until a script asks, so a driver sends its whole preamble
 * without waiting for anything, ends it with `reply`, and reads the ok for
 * that line as the signal the machine is now answering.
 *
 * A command that blocks answers when it finishes, not when it parses — the
 * reply for `run 600` comes six hundred frames later. That is the whole point
 * of the channel: a driver that saw it sooner would race the machine. */
static bool scr_replies;
static bool scr_answered; /* the finished command replied for itself */
static bool scr_pending;  /* a command is running and owes an answer */

static void scr_reply(const char *fmt, ...)
{
    if (!scr_replies)
        return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
    scr_answered = true;
}

static bool scr_error(const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "rp6502-emu: %s:%d: %s\n", scr_path, scr_line_no, msg);
    scr_reply("fail %s", msg);
    scr_pending = false;
    scr_fail = true;
    scr_run = false;
    return false;
}

/* Whether anything but a comment is left on the line. A quoted string is read
 * by scr_string before this ever sees it, so a '#' inside one is safe. */
static bool scr_more(char **p)
{
    char *s = *p;
    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '#')
        *s = 0;
    *p = s;
    return *s != 0;
}

/* The next whitespace-delimited word, terminated in place. NULL at end of line. */
static char *scr_word(char **p)
{
    char *s = *p;
    while (*s == ' ' || *s == '\t')
        s++;
    if (!*s || *s == '#')
    {
        *p = s;
        return NULL;
    }
    char *word = s;
    while (*s && *s != ' ' && *s != '\t')
        s++;
    if (*s)
        *s++ = 0;
    *p = s;
    return word;
}

/* A double-quoted argument with \n \r \t \\ \" escapes. Text is always quoted
 * so a trailing space or an empty string means what it says. */
static bool scr_string(char **p, char *out, size_t outsz)
{
    char *s = *p;
    while (*s == ' ' || *s == '\t')
        s++;
    if (*s != '"')
        return false;
    s++;
    size_t n = 0;
    while (*s && *s != '"')
    {
        char c = *s++;
        if (c == '\\' && *s)
        {
            char esc = *s++;
            c = esc == 'n' ? '\n' : esc == 'r' ? '\r' : esc == 't' ? '\t' : esc;
        }
        if (n + 1 >= outsz)
            return false;
        out[n++] = c;
    }
    if (*s != '"')
        return false;
    out[n] = 0;
    *p = s + 1;
    return true;
}

/* Strip a $ or 0x radix prefix, answering the base to read the rest in. */
static int scr_radix(char **w)
{
    if (**w == '$')
        return ++*w, 16;
    if ((*w)[0] == '0' && ((*w)[1] == 'x' || (*w)[1] == 'X'))
        return *w += 2, 16;
    return 10;
}

static bool scr_number(char **p, long *out)
{
    char *word = scr_word(p);
    if (!word)
        return false;
    bool neg = *word == '-';
    if (neg)
        word++;
    int base = scr_radix(&word);
    char *end;
    long value = strtol(word, &end, base);
    if (end == word || *end)
        return false;
    *out = neg ? -value : value;
    return true;
}

/* An address, in RAM unless it says xram:. */
static bool scr_address(char **p, uint8_t **base, long *addr)
{
    char *word = scr_word(p);
    if (!word)
        return false;
    *base = ram;
    if (!strncasecmp(word, "xram:", 5))
        *base = (uint8_t *)xram, word += 5;
    else if (!strncasecmp(word, "ram:", 4))
        word += 4;
    int radix = scr_radix(&word);
    char *end;
    long value = strtol(word, &end, radix);
    if (end == word || *end || value < 0 || value > 0xFFFF)
        return false;
    *addr = value;
    return true;
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static void scr_pad_publish(int player)
{
    pad_connect(player, true, scr_pad[player].type, scr_pad[player].sticks);
    pad_host_report(player, scr_pad[player].dpad, scr_pad[player].button0,
                    scr_pad[player].button1, scr_pad[player].lx, scr_pad[player].ly,
                    scr_pad[player].rx, scr_pad[player].ry, scr_pad[player].lt,
                    scr_pad[player].rt);
}

static bool scr_canvas_crc(uint32_t *out)
{
    const uint32_t *fb = vga_get_framebuffer();
    if (!fb)
        return false;
    int w, h;
    vga_canvas_size(&w, &h);
    *out = mem_crc32(0, fb, (size_t)w * h * 4);
    return true;
}

static bool scr_cmd_pad(char *p)
{
    long player;
    if (!scr_number(&p, &player) || player < 0 || player > 3)
        return scr_error("pad wants a player 0-3");
    char *verb = scr_word(&p);
    if (!verb)
        return scr_error("pad wants connect, disconnect, press, release, stick or trigger");
    if (!strcasecmp(verb, "connect") || !strcasecmp(verb, "disconnect"))
    {
        bool connect = !strcasecmp(verb, "connect");
        memset(&scr_pad[player], 0, sizeof scr_pad[player]);
        scr_pad[player].connected = connect;
        /* What a real host would know about the pad it found, if anything. */
        char *word;
        while (connect && (word = scr_word(&p)) != NULL)
        {
            if (!strcasecmp(word, "sticks"))
                scr_pad[player].sticks = true;
            else if (!strcasecmp(word, "western"))
                scr_pad[player].type = PAD_TYPE_WESTERN;
            else if (!strcasecmp(word, "eastern"))
                scr_pad[player].type = PAD_TYPE_EASTERN;
            else if (!strcasecmp(word, "playstation"))
                scr_pad[player].type = PAD_TYPE_PLAYSTATION;
            else
                return scr_error("pad connect wants western, eastern, "
                                 "playstation or sticks, not '%s'",
                                 word);
        }
        if (connect)
            scr_pad_publish((int)player); /* the claim lands now, not on first press */
        else
            pad_connect((int)player, false, PAD_TYPE_UNKNOWN, false);
        return true;
    }
    if (!scr_pad[player].connected)
        return scr_error("pad %ld is not connected", player);
    if (!strcasecmp(verb, "press") || !strcasecmp(verb, "release"))
    {
        bool down = !strcasecmp(verb, "press");
        char *name;
        int count = 0;
        while ((name = scr_word(&p)) != NULL)
        {
            pad_button_t button;
            if (!pad_button_from_name(name, &button))
                return scr_error("unknown pad button '%s'", name);
            pad_button_apply(button, down, &scr_pad[player].dpad,
                             &scr_pad[player].button0, &scr_pad[player].button1);
            count++;
        }
        if (!count)
            return scr_error("pad %s wants a button name", verb);
    }
    else if (!strcasecmp(verb, "stick"))
    {
        long v[4];
        for (int i = 0; i < 4; i++)
            if (!scr_number(&p, &v[i]) || v[i] < -128 || v[i] > 127)
                return scr_error("pad stick wants lx ly rx ry, each -128..127");
        scr_pad[player].lx = (int)v[0];
        scr_pad[player].ly = (int)v[1];
        scr_pad[player].rx = (int)v[2];
        scr_pad[player].ry = (int)v[3];
    }
    else if (!strcasecmp(verb, "trigger"))
    {
        long v[2];
        for (int i = 0; i < 2; i++)
            if (!scr_number(&p, &v[i]) || v[i] < 0 || v[i] > 255)
                return scr_error("pad trigger wants lt rt, each 0..255");
        scr_pad[player].lt = (int)v[0];
        scr_pad[player].rt = (int)v[1];
    }
    else
        return scr_error("unknown pad verb '%s'", verb);
    scr_pad_publish((int)player);
    return true;
}

static bool scr_cmd_mouse(char *p)
{
    char *verb = scr_word(&p);
    if (verb && !strcasecmp(verb, "move"))
    {
        long dx, dy;
        if (!scr_number(&p, &dx) || !scr_number(&p, &dy))
            return scr_error("mouse move wants dx dy");
        mou_host_move((float)dx, (float)dy);
        return true;
    }
    if (verb && !strcasecmp(verb, "wheel"))
    {
        long wheel, pan = 0;
        if (!scr_number(&p, &wheel))
            return scr_error("mouse wheel wants a count");
        if (scr_more(&p) && !scr_number(&p, &pan))
            return scr_error("mouse wheel wants a pan count");
        mou_host_wheel((int)wheel, (int)pan);
        return true;
    }
    if (verb && !strcasecmp(verb, "buttons"))
    {
        long mask;
        if (!scr_number(&p, &mask) || mask < 0 || mask > 255)
            return scr_error("mouse buttons wants a bitmap 0..255");
        mou_host_buttons((uint8_t)mask);
        return true;
    }
    return scr_error("mouse wants move, wheel or buttons");
}

static bool scr_cmd_tablet(char *p)
{
    char *verb = scr_word(&p);
    if (verb && !strcasecmp(verb, "at"))
    {
        long x, y, buttons = 0;
        if (!scr_number(&p, &x) || !scr_number(&p, &y))
            return scr_error("tablet at wants x y");
        if (scr_more(&p) && (!scr_number(&p, &buttons) || buttons < 0 || buttons > 255))
            return scr_error("tablet at wants a button bitmap 0..255");
        tab_host_pointer((int)x, (int)y, (uint8_t)buttons);
        return true;
    }
    if (verb && !strcasecmp(verb, "touch"))
    {
        tab_point_t points[TAB_MAX_CONTACTS];
        int count = 0;
        char *word;
        while ((word = scr_word(&p)) != NULL)
        {
            if (count == TAB_MAX_CONTACTS)
                return scr_error("tablet touch takes at most %d contacts", TAB_MAX_CONTACTS);
            char *comma = strchr(word, ',');
            if (!comma)
                return scr_error("tablet touch wants x,y pairs");
            *comma = 0;
            char *xs = word, *ys = comma + 1;
            long x, y;
            if (!scr_number(&xs, &x) || !scr_number(&ys, &y))
                return scr_error("tablet touch wants x,y pairs");
            points[count].x = (int16_t)x;
            points[count].y = (int16_t)y;
            count++;
        }
        tab_host_touch(points, count);
        return true;
    }
    if (verb && !strcasecmp(verb, "wheel"))
    {
        long wheel, pan = 0;
        if (!scr_number(&p, &wheel))
            return scr_error("tablet wheel wants a count");
        if (scr_more(&p) && !scr_number(&p, &pan))
            return scr_error("tablet wheel wants a pan count");
        tab_host_wheel((int)wheel, (int)pan);
        return true;
    }
    if (verb && !strcasecmp(verb, "clear"))
    {
        tab_host_clear();
        return true;
    }
    return scr_error("tablet wants at, touch, wheel or clear");
}

bool scr_command(const char *line)
{
    char buf[SCR_LINE_MAX];
    snprintf(buf, sizeof buf, "%s", line);
    char *end = buf + strlen(buf);
    while (end > buf && (end[-1] == '\n' || end[-1] == '\r' ||
                         end[-1] == ' ' || end[-1] == '\t'))
        *--end = 0;

    char *p = buf;
    char *cmd = scr_word(&p);
    if (!cmd || cmd[0] == '#')
        return true;

    if (!strcasecmp(cmd, "run"))
    {
        long frames = 1;
        if (scr_more(&p) && (!scr_number(&p, &frames) || frames < 0))
            return scr_error("run wants a frame count");
        scr_wait = SCR_FRAMES;
        scr_budget = (unsigned long)frames;
        return true;
    }

    if (!strcasecmp(cmd, "wait"))
    {
        /* Text or a byte in memory. scr_string does not consume the word when
         * it finds no quote, so trying it first costs nothing and the two
         * grammars cannot be confused for each other. */
        char text[sizeof scr_needle];
        if (scr_string(&p, text, sizeof text))
        {
            if (!text[0])
                return scr_error("wait wants a quoted string");
            snprintf(scr_needle, sizeof scr_needle, "%s", text);
            scr_wait = SCR_TEXT;
        }
        else
        {
            long want;
            if (!scr_address(&p, &scr_addr_base, &scr_addr) ||
                !scr_number(&p, &want) || want < 0 || want > 0xFF)
                return scr_error("wait wants a quoted string, or an address "
                                 "and the byte to wait for");
            scr_addr_want = (uint8_t)want;
            scr_wait = SCR_BYTE;
        }
        long frames = SCR_WAIT_FRAMES;
        if (scr_more(&p) && (!scr_number(&p, &frames) || frames < 0))
            return scr_error("wait wants a frame budget");
        scr_budget = (unsigned long)frames;
        return true;
    }

    if (!strcasecmp(cmd, "expect") || !strcasecmp(cmd, "expect-not"))
    {
        bool want = !strcasecmp(cmd, "expect");
        char text[256];
        if (!scr_string(&p, text, sizeof text) || !text[0])
            return scr_error("%s wants a quoted string", cmd);
        char *found = strstr(scr_cap, text);
        if (want && !found)
            return scr_error("expected \"%s\"", text);
        if (!want && found)
            return scr_error("did not expect \"%s\"", text);
        if (found)
            scr_cap_take(found + strlen(text));
        return true;
    }

    if (!strcasecmp(cmd, "expect-exit"))
    {
        long code, frames = SCR_WAIT_FRAMES;
        if (!scr_number(&p, &code))
            return scr_error("expect-exit wants an exit code");
        if (scr_more(&p) && (!scr_number(&p, &frames) || frames < 0))
            return scr_error("expect-exit wants a frame budget");
        scr_exit_want = (int)code;
        scr_wait = SCR_EXIT;
        scr_budget = (unsigned long)frames;
        return true;
    }

    if (!strcasecmp(cmd, "type"))
    {
        char text[SCR_LINE_MAX];
        if (!scr_string(&p, text, sizeof text))
            return scr_error("type wants a quoted string");
        long frames = SCR_WAIT_FRAMES;
        if (scr_more(&p) && (!scr_number(&p, &frames) || frames < 0))
            return scr_error("type wants a frame budget");
        kbd_paste(text);
        /* Blocks until the ring has it all, so back-to-back type commands
         * cannot replace each other mid-drip. */
        scr_wait = SCR_TYPING;
        scr_budget = (unsigned long)frames;
        return true;
    }

    if (!strcasecmp(cmd, "key"))
    {
        char *name = scr_word(&p);
        if (!name)
            return scr_error("key wants a key name");
        bool ctrl = false, shift = false, alt = false;
        char *plus;
        while ((plus = strrchr(name, '+')) != NULL)
        {
            *plus = 0;
            if (!strcasecmp(plus + 1, "ctrl"))
                ctrl = true;
            else if (!strcasecmp(plus + 1, "shift"))
                shift = true;
            else if (!strcasecmp(plus + 1, "alt"))
                alt = true;
            else
                return scr_error("unknown modifier '%s'", plus + 1);
        }
        uint8_t hid = kbd_hid_from_name(name);
        if (!hid || !kbd_key(hid, ctrl, shift, alt))
            return scr_error("'%s' has no key sequence", name);
        return true;
    }

    if (!strcasecmp(cmd, "press") || !strcasecmp(cmd, "release"))
    {
        bool down = !strcasecmp(cmd, "press");
        char *name;
        int count = 0;
        while ((name = scr_word(&p)) != NULL)
        {
            uint8_t hid = kbd_hid_from_name(name);
            if (!hid)
            {
                char *num = name;
                long usage;
                if (!scr_number(&num, &usage) || usage < 4 || usage > 255)
                    return scr_error("unknown key '%s'", name);
                hid = (uint8_t)usage;
            }
            kbd_hid_set(hid, down);
            count++;
        }
        if (!count)
            return scr_error("%s wants a key name", cmd);
        return true;
    }

    if (!strcasecmp(cmd, "lock"))
    {
        char *which = scr_word(&p);
        if (!which)
            which = "";
        if (!strcasecmp(which, "num"))
            kbd_toggle_lock(1);
        else if (!strcasecmp(which, "caps"))
            kbd_toggle_lock(2);
        else if (!strcasecmp(which, "scroll"))
            kbd_toggle_lock(4);
        else
            return scr_error("lock wants num, caps or scroll");
        return true;
    }

    if (!strcasecmp(cmd, "pad"))
        return scr_cmd_pad(p);
    if (!strcasecmp(cmd, "mouse"))
        return scr_cmd_mouse(p);
    if (!strcasecmp(cmd, "tablet"))
        return scr_cmd_tablet(p);

    if (!strcasecmp(cmd, "peek"))
    {
        uint8_t *base;
        long addr, want;
        if (!scr_address(&p, &base, &addr))
            return scr_error("peek wants an address");
        int i = 0;
        while (scr_more(&p))
        {
            if (!scr_number(&p, &want) || want < 0 || want > 255)
                return scr_error("peek wants byte values 0..255");
            if (addr + i > 0xFFFF)
                return scr_error("peek runs past the end of memory");
            if (base[addr + i] != (uint8_t)want)
                return scr_error("$%04lX+%d is $%02X, expected $%02lX",
                                 addr, i, base[addr + i], want);
            i++;
        }
        if (!i)
            return scr_error("peek wants at least one byte value");
        return true;
    }

    /* Writes land in ram[]/xram[] only. The VIA and the RIA answer the bus, not
     * memory, so a poke changes what the program reads and triggers nothing. */
    if (!strcasecmp(cmd, "poke"))
    {
        uint8_t *base;
        long addr, value;
        if (!scr_address(&p, &base, &addr))
            return scr_error("poke wants an address");
        int i = 0;
        while (scr_more(&p))
        {
            if (!scr_number(&p, &value) || value < 0 || value > 255)
                return scr_error("poke wants byte values 0..255");
            if (addr + i > 0xFFFF)
                return scr_error("poke runs past the end of memory");
            base[addr + i] = (uint8_t)value;
            i++;
        }
        if (!i)
            return scr_error("poke wants at least one byte value");
        return true;
    }

    if (!strcasecmp(cmd, "reply"))
    {
        bool on = true;
        char *word = scr_word(&p);
        if (word && !strcasecmp(word, "off"))
            on = false;
        else if (word && strcasecmp(word, "on"))
            return scr_error("reply wants on or off");
        scr_replies = on;
        return true;
    }

    if (!strcasecmp(cmd, "dump"))
    {
        uint8_t *base;
        long addr, count = 1;
        if (!scr_address(&p, &base, &addr))
            return scr_error("dump wants an address");
        if (scr_more(&p) && (!scr_number(&p, &count) || count < 1))
            return scr_error("dump wants a byte count");
        if (scr_replies)
            printf("ok");
        for (long i = 0; i < count && addr + i <= 0xFFFF; i++)
            printf("%s%02X", i || scr_replies ? " " : "", base[addr + i]);
        printf("\n");
        fflush(stdout);
        scr_answered = scr_replies;
        return true;
    }

    /* The canvas is checked against a remembered hash, never a literal one:
     * a hash written into a script is invalidated by every unrelated
     * rendering change. A driver that wants the number reads `crc`. */
    if (!strcasecmp(cmd, "crc") || !strcasecmp(cmd, "mark") ||
        !strcasecmp(cmd, "expect-same") || !strcasecmp(cmd, "expect-changed"))
    {
        uint32_t crc;
        if (!scr_canvas_crc(&crc))
            return scr_error("no framebuffer to hash");
        if (!strcasecmp(cmd, "crc"))
        {
            printf(scr_replies ? "ok %08X\n" : "%08X\n", crc);
            fflush(stdout);
            scr_answered = scr_replies;
            return true;
        }
        if (!strcasecmp(cmd, "mark"))
        {
            scr_mark = crc;
            scr_marked = true;
            return true;
        }
        if (!scr_marked)
            return scr_error("%s wants a mark first", cmd);
        bool same = crc == scr_mark;
        if (same != !strcasecmp(cmd, "expect-same"))
            return scr_error("the canvas %s since the mark", same ? "has not changed" : "changed");
        return true;
    }

    if (!strcasecmp(cmd, "shot"))
    {
        char path[512];
        if (!scr_string(&p, path, sizeof path))
            return scr_error("shot wants a quoted path");
        const uint32_t *fb = vga_get_framebuffer();
        if (!fb)
            return scr_error("no framebuffer to write");
        int w, h;
        vga_canvas_size(&w, &h);
        if (!png_write(path, w, h, fb))
            return scr_error("cannot write '%s'", path);
        return true;
    }

    return scr_error("unknown command '%s'", cmd);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/* Spend one of the pending wait's frames. False once the budget is gone. */
static bool scr_spend(void)
{
    if (!scr_budget)
        return false;
    scr_budget--;
    return true;
}

/* True once whatever the script is waiting for has happened. A budget that runs
 * out fails the run here rather than hanging it. */
static bool scr_settle(void)
{
    switch (scr_wait)
    {
    case SCR_IDLE:
        return true;
    case SCR_FRAMES:
        if (scr_spend())
            return false;
        break;
    case SCR_TEXT:
    {
        char *found = strstr(scr_cap, scr_needle);
        if (found)
        {
            scr_cap_take(found + strlen(scr_needle));
            break;
        }
    }
        if (scr_spend())
            return false;
        scr_show_capture();
        return scr_error("timed out waiting for \"%s\"", scr_needle);
    case SCR_BYTE:
        if (scr_addr_base[scr_addr] == scr_addr_want)
            break;
        if (scr_spend())
            return false;
        return scr_error("timed out waiting for %s:$%04lX to read $%02X; it is $%02X",
                         scr_addr_base == xram ? "xram" : "ram", scr_addr,
                         scr_addr_want, scr_addr_base[scr_addr]);
    case SCR_TYPING:
        if (!kbd_paste_busy())
            break;
        if (scr_spend())
            return false;
        return scr_error("timed out typing; the program is not reading its input");
    case SCR_EXIT:
        if (cpu_halted())
        {
            int code = proc_get_exit_code();
            if (code != scr_exit_want)
                return scr_error("exit code %d, expected %d", code, scr_exit_want);
            break;
        }
        if (scr_spend())
            return false;
        return scr_error("timed out waiting for the program to exit");
    }
    scr_wait = SCR_IDLE;
    return true;
}

/* Run the script forward until it owes the machine a frame, which is the only
 * reason it hands control back. The caller runs exactly one frame per return,
 * so a wait costs the frames it asked for and not one more. */
/* Whether the frame the script is owed is one it could look at. A run's frames
 * are seen only through the command after them, so all but the last can skip
 * the per-scanline pixel work — around a tenth of what a frame costs, the rest
 * being the 6502 and the raster itself. The blocking waits render every frame,
 * because which one settles them is not known until it has run. */
bool scr_needs_pixels(void)
{
    return !(scr_wait == SCR_FRAMES && scr_budget > 0);
}

void scr_task(void)
{
    char line[SCR_LINE_MAX];
    while (scr_run && scr_settle())
    {
        /* Settling is what says the last command is done, so this is where it
         * is answered — after the frames it asked for, not when it parsed. */
        if (scr_pending)
        {
            if (!scr_answered)
                scr_reply("ok");
            scr_pending = scr_answered = false;
        }
        if (!fgets(line, sizeof line, scr_file))
        {
            scr_run = false; /* end of script */
            return;
        }
        scr_line_no++;
        scr_pending = true;
        if (!scr_command(line))
            return; /* scr_error ended the run */
    }
}

/* The verbs, printed beside where they are implemented. A second copy in
 * cli.c is a second thing to remember, and the one that gets forgotten is
 * the copy nobody is reading while they change the parser. */
void scr_usage(FILE *out)
{
    fprintf(out,
            "\nscript commands (one per line, # comments, text always quoted;\n"
            "a failed check names the line and exits 1):\n"
            "  run [frames]              let frames elapse (default 1)\n"
            "  wait \"text\" [frames]      run until the console says it (default 600)\n"
            "  wait [xram:]<addr> <byte> [frames]  run until that byte reads that\n"
            "  type \"text\" [frames]      type it (\\n = Enter, \\t = Tab)\n"
            "  key <name>[+ctrl][+shift][+alt]   send a key's escape sequence\n"
            "  press/release <key>...    the direct HID bitmap, by name or 0xNN\n"
            "  lock num|caps|scroll      toggle a lock LED\n"
            "  pad <n> connect [western|eastern|playstation] [sticks] | disconnect\n"
            "  pad <n> press|release <button>...   a b c x y z l1 r1 l2 r2 l3 r3\n"
            "                                      select start home up down left right\n"
            "  pad <n> stick <lx> <ly> <rx> <ry>   -128..127\n"
            "  pad <n> trigger <lt> <rt>           0..255\n"
            "  mouse move <dx> <dy> | wheel <n> [pan] | buttons <mask>\n"
            "  tablet at <x> <y> [buttons] | touch <x>,<y>... | wheel <n> [pan] | clear\n"
            "  expect \"text\" / expect-not \"text\"   the console since the last check\n"
            "  expect-exit <code> [frames]         run until it exits, check the code\n"
            "  peek [xram:]<addr> <byte>...        compare memory\n"
            "  poke [xram:]<addr> <byte>...        write memory\n"
            "  dump [xram:]<addr> [count]          print memory as hex\n"
            "  crc                                 the canvas as a CRC-32\n"
            "  mark, expect-same, expect-changed   the canvas against a remembered one\n"
            "  shot \"file.png\"           write the canvas\n"
            "  reply [on|off]            answer every command on stdout, for a\n"
            "                            driver on the other end of a pipe\n");
}

bool scr_load(const char *path)
{
    if (!strcmp(path, "-"))
    {
        scr_file = stdin;
        scr_path = "<stdin>";
    }
    else
    {
        scr_file = fopen(path, "r");
        if (!scr_file)
        {
            fprintf(stderr, "rp6502-emu: cannot open script '%s'\n", path);
            return false;
        }
        scr_path = path;
    }
    com_set_tx_tap(scr_tap);
    /* Arm a clean run: a load inherits nothing from a script that ran before it,
     * not a half-finished wait, not console text nobody matched, not a verdict. */
    scr_line_no = 0;
    scr_wait = SCR_IDLE;
    scr_budget = 0;
    scr_cap_len = 0;
    scr_cap[0] = 0;
    scr_marked = false;
    scr_fail = false;
    scr_replies = false;
    scr_answered = false;
    scr_pending = false;
    scr_run = true;
    return true;
}

bool scr_loaded(void)
{
    return scr_file != NULL;
}

bool scr_running(void)
{
    return scr_run;
}

int scr_exit_code(void)
{
    return scr_fail ? 1 : 0;
}
