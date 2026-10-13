/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "host/sokol/cli/script.h"
#include "host/sokol/cli/png.h"
#include "host/sokol/cli/state.h"
#include "core/api/api.h"
#include "core/rom/rom.h"
#include "core/str/oem.h"
#include "core/sys/sys.h"
#include "host/host.h"
#include "core/api/proc.h"
#include "core/sys/proc.h"
#include "core/hid/keyboard.h"
#include "core/hid/usage.h"
#include "core/hid/vtkeys.h"
#include "core/hid/mouse.h"
#include "core/hid/gamepad.h"
#include "core/hid/tablet.h"
#include "core/com/com.h"
#include "core/wdc/cpu.h"
#include "core/wdc/sram.h"
#include "core/wdc/via.h"
#include "core/ria/ria.h"
#include "core/sys/xram.h"
#include "core/vga/vga_emu.h"
#include "osal/os.h"
#include "chips/chips/w65c02.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define SCRIPT_LINE_MAX 1024
#define SCRIPT_CAP_SIZE 65536
#define SCRIPT_CAP_KEEP 4096
#define SCRIPT_WAIT_FRAMES 600

typedef enum
{
    SCRIPT_IDLE,
    SCRIPT_FRAMES,
    SCRIPT_TEXT,
    SCRIPT_BYTE,
    SCRIPT_TYPING,
    SCRIPT_EXIT,
} script_wait_t;

static FILE *script_file;
static const char *script_path = "<script>"; /* until a file is opened */
static int script_line_no;
static bool script_run, script_fail;

static script_wait_t script_wait;
static unsigned long script_budget;
static char script_needle[256];
static size_t script_needle_len;
static int script_exit_want;

/* Where an address points. Without a prefix, or with ram:, it is the 6502's
 * address space. bus: is the same space, written the way the 6502 writes it. */
typedef enum
{
    SCRIPT_CPU,
    SCRIPT_XRAM,
    SCRIPT_BUS,
} script_space_t;

/* A `wait` on memory looks here. script_settle samples the byte once a
 * frame, so a program that publishes a value for a few cycles and moves on can
 * be missed. */
static script_space_t script_addr_space;
static long script_addr;
static uint8_t script_addr_want;

static uint32_t script_mark;
static bool script_marked;

static struct
{
    bool connected;
    bool sticks;
    uint8_t type;
    uint8_t dpad, button0, button1;
    int lx, ly, rx, ry, lt, rt;
} script_gamepad[4];

/* What the program wrote to stdout and to stderr that no check has used yet,
 * converted from the code page to UTF-8 as it was written, so that it compares
 * with the script's own text. The line editor's echo of typed text is not the
 * program's output, so it is in neither. */
typedef struct
{
    char buf[SCRIPT_CAP_SIZE];
    size_t len;
} script_cap_t;

static script_cap_t script_cap[2];
#define SCRIPT_STDOUT (&script_cap[0])
#define SCRIPT_STDERR (&script_cap[1])

/* The text a `wait` is looking in. */
static script_cap_t *script_wait_cap;

/* The program's stderr reaches the host's as it is written. This says whether
 * it stopped partway through a line. */
static bool script_stderr_midline;

static void script_std_tap(int fd, const char *buf, int len)
{
    if (fd != 1 && fd != 2)
        return;
    if (fd == 2 && len)
        script_stderr_midline = buf[len - 1] != '\n';
    script_cap_t *cap = &script_cap[fd - 1];
    for (int i = 0; i < len; i++)
    {
        char utf8[3];
        size_t n = (size_t)oem_to_utf8_char((unsigned char)buf[i], utf8);
        if (cap->len + n > sizeof cap->buf)
        {
            /* Only output that no check has taken yet gets this far. The
             * newest SCRIPT_CAP_KEEP bytes are kept so that a needle
             * straddling the discard still has somewhere to match, starting
             * on a whole character. */
            size_t from = cap->len - SCRIPT_CAP_KEEP;
            while (((unsigned char)cap->buf[from] & 0xC0) == 0x80)
                from++;
            memmove(cap->buf, cap->buf + from, cap->len - from);
            cap->len -= from;
        }
        memcpy(cap->buf + cap->len, utf8, n);
        cap->len += n;
    }
}

/* Where the text first appears in the capture, or NULL. The program can write
 * a zero byte, so the search counts lengths rather than stopping at one. */
static char *script_find(script_cap_t *cap, const char *text, size_t len)
{
    char *end = cap->buf + cap->len;
    for (char *s = cap->buf; (size_t)(end - s) >= len; s++)
    {
        s = memchr(s, text[0], (size_t)(end - s) - len + 1);
        if (!s)
            return NULL;
        if (!memcmp(s, text, len))
            return s;
    }
    return NULL;
}

static void script_cap_take(script_cap_t *cap, const char *through)
{
    size_t used = (size_t)(through - cap->buf);
    memmove(cap->buf, cap->buf + used, cap->len - used);
    cap->len -= used;
}

/* An optional stdout or stderr before a string. Without one it is stdout. */
static script_cap_t *script_stream(char **p)
{
    char *s = *p;
    while (*s == ' ' || *s == '\t')
        s++;
    size_t n = strcspn(s, " \t");
    script_cap_t *cap = NULL;
    if (n == 6 && !strncasecmp(s, "stdout", 6))
        cap = SCRIPT_STDOUT;
    else if (n == 6 && !strncasecmp(s, "stderr", 6))
        cap = SCRIPT_STDERR;
    if (!cap)
        return SCRIPT_STDOUT;
    *p = s + n;
    return cap;
}

/* Messages name the stream only when it is not the default. */
static const char *script_on(const script_cap_t *cap)
{
    return cap == SCRIPT_STDERR ? " on stderr" : "";
}

/* Where the program is: the 6502's registers, then the RIA's OP and BUSY.
 * RESB is 0 once the program has stopped and the 6502 is held in reset. */
static void script_cpu(char *buf, size_t size)
{
    w65c02_t *c = cpu_chip();
    snprintf(buf, size,
             "PC=$%04X A=$%02X X=$%02X Y=$%02X SP=$%02X P=$%02X OP=$%02X BUSY=$%02X RESB=%d",
             w65c02_pc(c), w65c02_a(c), w65c02_x(c), w65c02_y(c), w65c02_s(c),
             w65c02_p(c), API_OP, REGS(0xFFF2), sys_running() ? 1 : 0);
}

/* Control bytes are written as C escapes, so a failure is always one line. */
static void script_printable(char *out, size_t size, const char *in, size_t len)
{
    size_t n = 0;
    for (size_t i = 0; i < len && n + 5 < size; i++)
    {
        unsigned char c = (unsigned char)in[i];
        if (c == '\n')
            n += (size_t)snprintf(out + n, size - n, "\\n");
        else if (c == '\r')
            n += (size_t)snprintf(out + n, size - n, "\\r");
        else if (c == '\t')
            n += (size_t)snprintf(out + n, size - n, "\\t");
        else if (c < ' ' || c == 0x7F)
            n += (size_t)snprintf(out + n, size - n, "\\x%02X", c);
        else
            out[n++] = (char)c;
    }
    out[n] = 0;
}

static bool script_replies;
static bool script_answered; /* the finished command replied for itself */
static bool script_pending;  /* a command is running and owes an answer */

static void script_reply(const char *fmt, ...)
{
    if (!script_replies)
        return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
    script_answered = true;
}

/* Every failure ends with where the program is, because the run ends here and
 * a driver can't ask afterwards. */
static bool script_error(const char *fmt, ...)
{
    char msg[1024], text[4096], cpu[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    script_printable(text, sizeof text, msg, strlen(msg));
    script_cpu(cpu, sizeof cpu);
    script_reply("fail %s; %s", text, cpu);
    /* The program's stdout that no check has used goes out raw after the
     * failure, up to EOF. Its stderr is on stderr already. */
    fwrite(SCRIPT_STDOUT->buf, 1, SCRIPT_STDOUT->len, stdout);
    fflush(stdout);
    /* The message goes last, on a line of its own, so a program that clears
     * or resets the screen can't wipe it. */
    if ((SCRIPT_STDOUT->len && SCRIPT_STDOUT->buf[SCRIPT_STDOUT->len - 1] != '\n') ||
        script_stderr_midline)
        fputc('\n', stderr);
    fprintf(stderr, "rp6502-emu: %s:%d: %s; %s\n", script_path, script_line_no, text, cpu);
    script_pending = false;
    script_fail = true;
    script_run = false;
    return false;
}

/* Whether anything but a comment is left on the line. A quoted string is read
 * by script_string before this ever sees it, so a '#' inside one is safe. */
static bool script_more(char **p)
{
    char *s = *p;
    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '#')
        *s = 0;
    *p = s;
    return *s != 0;
}

static char *script_word(char **p)
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

static int script_hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* The C escapes, so that a script can put any byte in a string. Hex takes one
 * or two digits and octal one to three, where C lets \x run as far as there
 * are digits, because a script writes a byte and then more text. An escape C
 * does not define is the character itself, which is what makes \" and \\
 * work. */
static char script_escape(char **p)
{
    char esc = *(*p)++;
    switch (esc)
    {
    case 'a':
        return '\a';
    case 'b':
        return '\b';
    case 'f':
        return '\f';
    case 'n':
        return '\n';
    case 'r':
        return '\r';
    case 't':
        return '\t';
    case 'v':
        return '\v';
    case 'x':
    {
        int value = 0, digits = 0;
        for (; digits < 2 && script_hex_digit(**p) >= 0; digits++)
            value = value * 16 + script_hex_digit(*(*p)++);
        return digits ? (char)value : esc;
    }
    default:
        if (esc >= '0' && esc <= '7')
        {
            int value = esc - '0';
            for (int digits = 1; digits < 3 && **p >= '0' && **p <= '7'; digits++)
                value = value * 8 + (*(*p)++ - '0');
            return (char)value;
        }
        return esc;
    }
}

/* A quoted string and its length, which counts any \0 written in it. */
static bool script_text(char **p, char *out, size_t outsz, size_t *len)
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
            c = script_escape(&s);
        if (n + 1 >= outsz)
            return false;
        out[n++] = c;
    }
    if (*s != '"')
        return false;
    out[n] = 0;
    *len = n;
    *p = s + 1;
    return true;
}

/* The paths and typed text from here use C strings, so a \0 would end one
 * early and send part of what was written. The command fails instead. */
static bool script_string(char **p, char *out, size_t outsz)
{
    size_t len;
    return script_text(p, out, outsz, &len) && strlen(out) == len;
}

static int script_radix(char **w)
{
    if (**w == '$')
        return ++*w, 16;
    if ((*w)[0] == '0' && ((*w)[1] == 'x' || (*w)[1] == 'X'))
        return *w += 2, 16;
    return 10;
}

static bool script_number(char **p, long *out)
{
    char *word = script_word(p);
    if (!word)
        return false;
    bool neg = *word == '-';
    if (neg)
        word++;
    int base = script_radix(&word);
    char *end;
    long value = strtol(word, &end, base);
    if (end == word || *end)
        return false;
    *out = neg ? -value : value;
    return true;
}

static bool script_address(char **p, script_space_t *space, long *addr)
{
    char *word = script_word(p);
    if (!word)
        return false;
    *space = SCRIPT_CPU;
    if (!strncasecmp(word, "xram:", 5))
        *space = SCRIPT_XRAM, word += 5;
    else if (!strncasecmp(word, "bus:", 4))
        *space = SCRIPT_BUS, word += 4;
    else if (!strncasecmp(word, "ram:", 4))
        word += 4;
    int radix = script_radix(&word);
    char *end;
    long value = strtol(word, &end, radix);
    if (end == word || *end || value < 0 || value > 0xFFFF)
        return false;
    *addr = value;
    return true;
}

/* Nothing answers a read of $FF00-$FFCF or takes a write there. */
static bool script_unassigned(script_space_t space, long addr)
{
    return space != SCRIPT_XRAM && addr > SRAM_MMAP_HI && addr < VIA_MMAP_LO;
}

/* What a read finds, with none of what a read on the bus does. */
static uint8_t script_peek(script_space_t space, uint16_t addr)
{
    if (space == SCRIPT_XRAM)
        return xram[addr];
    if (addr >= RIA_MMAP_LO)
        return REGS(addr);
    if (addr >= VIA_MMAP_LO)
        return via_reg_peek(addr);
    return sram[addr];
}

/* A plain poke sets what a read finds. bus: stores the way the 6502 does, so
 * a register does what a store to it does, and sram[] takes every byte the
 * way sram_tick does. */
static void script_poke(script_space_t space, uint16_t addr, uint8_t value)
{
    if (space == SCRIPT_XRAM)
        xram[addr] = value;
    else if (space == SCRIPT_BUS)
    {
        sram[addr] = value;
        if (addr >= RIA_MMAP_LO)
            ria_reg_write(addr, value);
        else if (addr >= VIA_MMAP_LO)
            via_reg_write(addr, value);
    }
    else if (addr >= RIA_MMAP_LO)
        REGS(addr) = value;
    else if (addr >= VIA_MMAP_LO)
        via_reg_poke(addr, value);
    else
        sram[addr] = value;
}

static void script_gamepad_publish(int player)
{
    gamepad_connect(player, true, script_gamepad[player].type, script_gamepad[player].sticks);
    gamepad_host_report(player, script_gamepad[player].dpad, script_gamepad[player].button0,
                        script_gamepad[player].button1, script_gamepad[player].lx, script_gamepad[player].ly,
                    script_gamepad[player].rx, script_gamepad[player].ry, script_gamepad[player].lt,
                    script_gamepad[player].rt);
}

static const struct
{
    const char *name;
    gamepad_button_t button;
} gamepad_names[] = {
    {"up", GAMEPAD_BTN_DPAD_UP},
    {"down", GAMEPAD_BTN_DPAD_DOWN},
    {"left", GAMEPAD_BTN_DPAD_LEFT},
    {"right", GAMEPAD_BTN_DPAD_RIGHT},
    {"a", GAMEPAD_BTN_A},
    {"b", GAMEPAD_BTN_B},
    {"c", GAMEPAD_BTN_C},
    {"x", GAMEPAD_BTN_X},
    {"y", GAMEPAD_BTN_Y},
    {"z", GAMEPAD_BTN_Z},
    {"l1", GAMEPAD_BTN_L1},
    {"r1", GAMEPAD_BTN_R1},
    {"l2", GAMEPAD_BTN_L2},
    {"r2", GAMEPAD_BTN_R2},
    {"select", GAMEPAD_BTN_SELECT},
    {"start", GAMEPAD_BTN_START},
    {"home", GAMEPAD_BTN_HOME},
    {"l3", GAMEPAD_BTN_L3},
    {"r3", GAMEPAD_BTN_R3},
};

static bool gamepad_button_from_name(const char *name, gamepad_button_t *button)
{
    if (!name)
        return false;
    for (size_t i = 0; i < sizeof gamepad_names / sizeof gamepad_names[0]; i++)
        if (!strcasecmp(name, gamepad_names[i].name))
        {
            *button = gamepad_names[i].button;
            return true;
        }
    return false;
}

static bool script_cmd_gamepad(char *p)
{
    long player;
    if (!script_number(&p, &player) || player < 0 || player > 3)
        return script_error("pad wants a player 0-3");
    char *verb = script_word(&p);
    if (!verb)
        return script_error("pad wants connect, disconnect, press, release, stick or trigger");
    if (!strcasecmp(verb, "connect") || !strcasecmp(verb, "disconnect"))
    {
        bool connect = !strcasecmp(verb, "connect");
        memset(&script_gamepad[player], 0, sizeof script_gamepad[player]);
        script_gamepad[player].connected = connect;
        char *word;
        while (connect && (word = script_word(&p)) != NULL)
        {
            if (!strcasecmp(word, "sticks"))
                script_gamepad[player].sticks = true;
            else if (!strcasecmp(word, "western"))
                script_gamepad[player].type = GAMEPAD_TYPE_WESTERN;
            else if (!strcasecmp(word, "eastern"))
                script_gamepad[player].type = GAMEPAD_TYPE_EASTERN;
            else if (!strcasecmp(word, "playstation"))
                script_gamepad[player].type = GAMEPAD_TYPE_PLAYSTATION;
            else
                return script_error("pad connect wants western, eastern, "
                                 "playstation or sticks, not '%s'",
                                 word);
        }
        if (connect)
            script_gamepad_publish((int)player);
        else
            gamepad_connect((int)player, false, GAMEPAD_TYPE_UNKNOWN, false);
        return true;
    }
    if (!script_gamepad[player].connected)
        return script_error("pad %ld is not connected", player);
    if (!strcasecmp(verb, "press") || !strcasecmp(verb, "release"))
    {
        bool down = !strcasecmp(verb, "press");
        char *name;
        int count = 0;
        while ((name = script_word(&p)) != NULL)
        {
            gamepad_button_t button;
            if (!gamepad_button_from_name(name, &button))
                return script_error("unknown pad button '%s'", name);
            gamepad_button_apply(button, down, &script_gamepad[player].dpad,
                                 &script_gamepad[player].button0, &script_gamepad[player].button1);
            count++;
        }
        if (!count)
            return script_error("pad %s wants a button name", verb);
    }
    else if (!strcasecmp(verb, "stick"))
    {
        long v[4];
        for (int i = 0; i < 4; i++)
            if (!script_number(&p, &v[i]) || v[i] < -128 || v[i] > 127)
                return script_error("pad stick wants lx ly rx ry, each -128..127");
        script_gamepad[player].lx = (int)v[0];
        script_gamepad[player].ly = (int)v[1];
        script_gamepad[player].rx = (int)v[2];
        script_gamepad[player].ry = (int)v[3];
    }
    else if (!strcasecmp(verb, "trigger"))
    {
        long v[2];
        for (int i = 0; i < 2; i++)
            if (!script_number(&p, &v[i]) || v[i] < 0 || v[i] > 255)
                return script_error("pad trigger wants lt rt, each 0..255");
        script_gamepad[player].lt = (int)v[0];
        script_gamepad[player].rt = (int)v[1];
    }
    else
        return script_error("unknown pad verb '%s'", verb);
    script_gamepad_publish((int)player);
    return true;
}

static bool script_cmd_mouse(char *p)
{
    char *verb = script_word(&p);
    if (verb && !strcasecmp(verb, "move"))
    {
        long dx, dy;
        if (!script_number(&p, &dx) || !script_number(&p, &dy))
            return script_error("mouse move wants dx dy");
        mouse_host_move(dx * MOUSE_ONE, dy * MOUSE_ONE);
        return true;
    }
    if (verb && !strcasecmp(verb, "wheel"))
    {
        long wheel, pan = 0;
        if (!script_number(&p, &wheel))
            return script_error("mouse wheel wants a count");
        if (script_more(&p) && !script_number(&p, &pan))
            return script_error("mouse wheel wants a pan count");
        mouse_host_wheel((int)wheel, (int)pan);
        return true;
    }
    if (verb && !strcasecmp(verb, "buttons"))
    {
        long mask;
        if (!script_number(&p, &mask) || mask < 0 || mask > 255)
            return script_error("mouse buttons wants a bitmap 0..255");
        mouse_host_buttons((uint8_t)mask);
        return true;
    }
    return script_error("mouse wants move, wheel or buttons");
}

static bool script_cmd_tablet(char *p)
{
    char *verb = script_word(&p);
    if (verb && !strcasecmp(verb, "at"))
    {
        long x, y, buttons = 0;
        if (!script_number(&p, &x) || !script_number(&p, &y))
            return script_error("tablet at wants x y");
        if (script_more(&p) && (!script_number(&p, &buttons) || buttons < 0 || buttons > 255))
            return script_error("tablet at wants a button bitmap 0..255");
        tablet_host_pointer((int)x, (int)y, (uint8_t)buttons, true);
        return true;
    }
    if (verb && !strcasecmp(verb, "touch"))
    {
        tablet_point_t points[TABLET_MAX_CONTACTS];
        int count = 0;
        char *word;
        while ((word = script_word(&p)) != NULL)
        {
            if (count == TABLET_MAX_CONTACTS)
                return script_error("tablet touch takes at most %d contacts", TABLET_MAX_CONTACTS);
            char *comma = strchr(word, ',');
            if (!comma)
                return script_error("tablet touch wants x,y pairs");
            *comma = 0;
            char *xs = word, *ys = comma + 1;
            long x, y;
            if (!script_number(&xs, &x) || !script_number(&ys, &y))
                return script_error("tablet touch wants x,y pairs");
            points[count].x = (int16_t)x;
            points[count].y = (int16_t)y;
            count++;
        }
        tablet_host_touch(points, count);
        return true;
    }
    if (verb && !strcasecmp(verb, "wheel"))
    {
        long wheel, pan = 0;
        if (!script_number(&p, &wheel))
            return script_error("tablet wheel wants a count");
        if (script_more(&p) && !script_number(&p, &pan))
            return script_error("tablet wheel wants a pan count");
        tablet_host_wheel((int)wheel, (int)pan);
        return true;
    }
    if (verb && !strcasecmp(verb, "clear"))
    {
        tablet_host_clear();
        return true;
    }
    return script_error("tablet wants at, touch, wheel or clear");
}

/* The keys with a name instead of a character, and what each one types on a
 * US keyboard without and with shift. Letters, digits, function keys and the
 * keypad are computed below rather than listed. A key that types nothing, and
 * the four vtkeys_key answers for itself, carry 0. Only the script language
 * takes a key as text, so the table lives here and not in core. */
static const struct
{
    const char *name;
    uint8_t hid;
    char plain;
    char shifted;
} script_named[] = {
    {"enter", 0x28, 0, 0},
    {"escape", 0x29, 0, 0},
    {"backspace", 0x2A, 0, 0},
    {"tab", 0x2B, 0, 0},
    {"up", 0x52, 0, 0},
    {"down", 0x51, 0, 0},
    {"left", 0x50, 0, 0},
    {"right", 0x4F, 0, 0},
    {"home", 0x4A, 0, 0},
    {"end", 0x4D, 0, 0},
    {"insert", 0x49, 0, 0},
    {"delete", 0x4C, 0, 0},
    {"pageup", 0x4B, 0, 0},
    {"pagedown", 0x4E, 0, 0},
    {"kpenter", 0x58, 0, 0},
    {"space", 0x2C, ' ', ' '},
    {"minus", 0x2D, '-', '_'},
    {"equal", 0x2E, '=', '+'},
    {"leftbracket", 0x2F, '[', '{'},
    {"rightbracket", 0x30, ']', '}'},
    {"backslash", 0x31, '\\', '|'},
    {"semicolon", 0x33, ';', ':'},
    {"apostrophe", 0x34, '\'', '"'},
    {"grave", 0x35, '`', '~'},
    {"comma", 0x36, ',', '<'},
    {"period", 0x37, '.', '>'},
    {"slash", 0x38, '/', '?'},
    {"capslock", 0x39, 0, 0},
    {"printscreen", 0x46, 0, 0},
    {"scrolllock", 0x47, 0, 0},
    {"pause", 0x48, 0, 0},
    {"numlock", 0x53, 0, 0},
    {"menu", 0x65, 0, 0},
    {"kpdivide", 0x54, '/', '/'},
    {"kpmultiply", 0x55, '*', '*'},
    {"kpsubtract", 0x56, '-', '-'},
    {"kpadd", 0x57, '+', '+'},
    {"kpdecimal", 0x63, '.', '.'},
    {"kpequal", 0x67, '=', '='},
    {"lctrl", 0xE0, 0, 0},
    {"lshift", 0xE1, 0, 0},
    {"lalt", 0xE2, 0, 0},
    {"lsuper", 0xE3, 0, 0},
    {"rctrl", 0xE4, 0, 0},
    {"rshift", 0xE5, 0, 0},
    {"ralt", 0xE6, 0, 0},
    {"rsuper", 0xE7, 0, 0},
};

static int script_fkey_num(const char *name)
{
    if (name[0] != 'f' && name[0] != 'F')
        return 0;
    int n = 0;
    const char *p = name + 1;
    if (!*p)
        return 0;
    for (; *p; p++)
    {
        if (*p < '0' || *p > '9')
            return 0;
        n = n * 10 + (*p - '0');
    }
    return (n >= 1 && n <= 12) ? n : 0;
}

/* The usage a name means, and the characters that key types. plain and
 * shifted may be NULL, and are 0 for a key that types nothing. */
static uint8_t script_key_from_name(const char *name, char *plain, char *shifted)
{
    static const char digit_shifted[] = ")!@#$%^&*(";
    if (plain)
        *plain = 0;
    if (shifted)
        *shifted = 0;
    if (!name || !name[0])
        return 0;
    if (!name[1])
    {
        char c = name[0];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c + 32);
        if (c >= 'a' && c <= 'z')
        {
            if (plain)
                *plain = c;
            if (shifted)
                *shifted = (char)(c - 32);
            return (uint8_t)(0x04 + c - 'a');
        }
        if (c >= '0' && c <= '9')
        {
            if (plain)
                *plain = c;
            if (shifted)
                *shifted = digit_shifted[c - '0'];
            return c == '0' ? 0x27 : (uint8_t)(0x1E + c - '1');
        }
        return 0;
    }
    if ((name[0] == 'k' || name[0] == 'K') && (name[1] == 'p' || name[1] == 'P') &&
        name[2] >= '0' && name[2] <= '9' && !name[3])
    {
        /* A keypad digit is a digit, because keyboard_init turns NumLock on. */
        if (plain)
            *plain = name[2];
        if (shifted)
            *shifted = name[2];
        return name[2] == '0' ? 0x62 : (uint8_t)(0x59 + name[2] - '1');
    }
    int f = script_fkey_num(name);
    if (f)
        return (uint8_t)(0x3A + f - 1);
    for (size_t i = 0; i < sizeof script_named / sizeof script_named[0]; i++)
        if (!strcasecmp(name, script_named[i].name))
        {
            if (plain)
                *plain = script_named[i].plain;
            if (shifted)
                *shifted = script_named[i].shifted;
            return script_named[i].hid;
        }
    return 0;
}

/* Run one command line. False on a bad line or a failed assertion, either of
 * which ends the run. */
static bool script_command(const char *line)
{
    char buf[SCRIPT_LINE_MAX];
    snprintf(buf, sizeof buf, "%s", line);
    char *end = buf + strlen(buf);
    while (end > buf && (end[-1] == '\n' || end[-1] == '\r' ||
                         end[-1] == ' ' || end[-1] == '\t'))
        *--end = 0;

    char *p = buf;
    char *cmd = script_word(&p);
    if (!cmd || cmd[0] == '#')
        return true;

    if (!strcasecmp(cmd, "run"))
    {
        long frames = 1;
        if (script_more(&p) && (!script_number(&p, &frames) || frames < 0))
            return script_error("run wants a frame count");
        script_wait = SCRIPT_FRAMES;
        script_budget = (unsigned long)frames;
        return true;
    }

    if (!strcasecmp(cmd, "wait"))
    {
        /* Text, or a byte in memory. script_text leaves the line alone when it
         * finds no quote, so trying the text form first costs nothing and the
         * two grammars cannot be confused. */
        char *before = p;
        script_cap_t *cap = script_stream(&p);
        if (script_text(&p, script_needle, sizeof script_needle, &script_needle_len))
        {
            if (!script_needle_len)
                return script_error("wait wants a quoted string");
            script_wait_cap = cap;
            script_wait = SCRIPT_TEXT;
        }
        else if (p != before)
            return script_error("wait %s wants a quoted string",
                                cap == SCRIPT_STDERR ? "stderr" : "stdout");
        else
        {
            long want;
            if (!script_address(&p, &script_addr_space, &script_addr) ||
                !script_number(&p, &want) || want < 0 || want > 0xFF)
                return script_error("wait wants a quoted string, or an address "
                                 "and the byte to wait for");
            if (script_addr_space == SCRIPT_BUS)
                return script_error("bus: is only for poke");
            if (script_unassigned(script_addr_space, script_addr))
                return script_error("$%04lX is unassigned", script_addr);
            script_addr_want = (uint8_t)want;
            script_wait = SCRIPT_BYTE;
        }
        long frames = SCRIPT_WAIT_FRAMES;
        if (script_more(&p) && (!script_number(&p, &frames) || frames < 0))
            return script_error("wait wants a frame budget");
        script_budget = (unsigned long)frames;
        return true;
    }

    if (!strcasecmp(cmd, "expect") || !strcasecmp(cmd, "expect-not"))
    {
        bool want = !strcasecmp(cmd, "expect");
        script_cap_t *cap = script_stream(&p);
        char text[256], shown[4 * sizeof text];
        size_t len;
        if (!script_text(&p, text, sizeof text, &len) || !len)
            return script_error("%s wants a quoted string", cmd);
        char *found = script_find(cap, text, len);
        script_printable(shown, sizeof shown, text, len);
        if (want && !found)
            return script_error("expected \"%s\"%s", shown, script_on(cap));
        if (!want && found)
            return script_error("did not expect \"%s\"%s", shown, script_on(cap));
        if (found)
            script_cap_take(cap, found + len);
        return true;
    }

    if (!strcasecmp(cmd, "expect-exit"))
    {
        long code, frames = SCRIPT_WAIT_FRAMES;
        if (!script_number(&p, &code))
            return script_error("expect-exit wants an exit code");
        if (script_more(&p) && (!script_number(&p, &frames) || frames < 0))
            return script_error("expect-exit wants a frame budget");
        script_exit_want = (int)code;
        script_wait = SCRIPT_EXIT;
        script_budget = (unsigned long)frames;
        return true;
    }

    if (!strcasecmp(cmd, "type"))
    {
        char text[SCRIPT_LINE_MAX];
        if (!script_string(&p, text, sizeof text))
            return script_error("type wants a quoted string");
        long frames = SCRIPT_WAIT_FRAMES;
        if (script_more(&p) && (!script_number(&p, &frames) || frames < 0))
            return script_error("type wants a frame budget");
        vtkeys_paste(text);
        /* Waiting for the text to drain, because vtkeys_paste replaces
         * whatever is still being delivered and back-to-back type commands
         * would otherwise truncate each other. */
        script_wait = SCRIPT_TYPING;
        script_budget = (unsigned long)frames;
        return true;
    }


    if (!strcasecmp(cmd, "key"))
    {
        char *name = script_word(&p);
        if (!name)
            return script_error("key wants a key name");
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
                return script_error("unknown modifier '%s'", plus + 1);
        }
        char plain, shifted;
        uint8_t hid = script_key_from_name(name, &plain, &shifted);
        if (!hid)
            return script_error("unknown key '%s'", name);
        if (vtkeys_key(hid, ctrl, shift, alt))
            return true;
        /* vtkeys_key is false for a key that types a character, which the
         * window hosts deliver as a chord built from the character their own
         * host resolved. A script has no host, so the character comes from
         * the table above. */
        char ch = shift ? shifted : plain;
        if (!ch)
            return script_error("'%s' sends nothing", name);
        if (ctrl && !alt)
        {
            /* Not vtkeys_ctrl_letter, which drops a key that has no control
             * byte. A key that sends nothing is an error instead. */
            char byte = keyboard_ctrl_promote(ch, HID_KEY_NONE);
            if (!byte)
                return script_error("'%s' has no control byte", name);
            com_keyboard_push_byte((uint8_t)byte);
        }
        else if (alt)
            vtkeys_alt_char(ch, ctrl);
        else
            com_keyboard_push_byte((uint8_t)ch);
        return true;
    }

    if (!strcasecmp(cmd, "press") || !strcasecmp(cmd, "release"))
    {
        bool down = !strcasecmp(cmd, "press");
        char *name;
        int count = 0;
        while ((name = script_word(&p)) != NULL)
        {
            uint8_t hid = script_key_from_name(name, NULL, NULL);
            if (!hid)
            {
                char *num = name;
                long usage;
                if (!script_number(&num, &usage) || usage < 4 || usage > 255)
                    return script_error("unknown key '%s'", name);
                hid = (uint8_t)usage;
            }
            keyboard_hid_set(hid, down);
            count++;
        }
        if (!count)
            return script_error("%s wants a key name", cmd);
        return true;
    }

    if (!strcasecmp(cmd, "lock"))
    {
        char *which = script_word(&p);
        if (!which)
            which = "";
        if (!strcasecmp(which, "num"))
            keyboard_toggle_lock(KEYBOARD_LED_NUMLOCK);
        else if (!strcasecmp(which, "caps"))
            keyboard_toggle_lock(KEYBOARD_LED_CAPSLOCK);
        else if (!strcasecmp(which, "scroll"))
            keyboard_toggle_lock(KEYBOARD_LED_SCROLLLOCK);
        else
            return script_error("lock wants num, caps or scroll");
        return true;
    }

    if (!strcasecmp(cmd, "pad"))
        return script_cmd_gamepad(p);
    if (!strcasecmp(cmd, "mouse"))
        return script_cmd_mouse(p);
    if (!strcasecmp(cmd, "tablet"))
        return script_cmd_tablet(p);

    if (!strcasecmp(cmd, "peek"))
    {
        script_space_t space;
        long addr, want;
        if (!script_address(&p, &space, &addr))
            return script_error("peek wants an address");
        if (space == SCRIPT_BUS)
            return script_error("bus: is only for poke");
        int i = 0;
        while (script_more(&p))
        {
            if (!script_number(&p, &want) || want < 0 || want > 255)
                return script_error("peek wants byte values 0..255");
            if (addr + i > 0xFFFF)
                return script_error("peek runs past the end of memory");
            if (script_unassigned(space, addr + i))
                return script_error("$%04lX is unassigned", addr + i);
            uint8_t got = script_peek(space, (uint16_t)(addr + i));
            if (got != (uint8_t)want)
                return script_error("$%04lX+%d is $%02X, expected $%02lX",
                                    addr, i, got, want);
            i++;
        }
        if (!i)
            return script_error("peek wants at least one byte value");
        return true;
    }

    if (!strcasecmp(cmd, "poke"))
    {
        script_space_t space;
        long addr, value;
        if (!script_address(&p, &space, &addr))
            return script_error("poke wants an address");
        int i = 0;
        while (script_more(&p))
        {
            if (!script_number(&p, &value) || value < 0 || value > 255)
                return script_error("poke wants byte values 0..255");
            if (addr + i > 0xFFFF)
                return script_error("poke runs past the end of memory");
            if (script_unassigned(space, addr + i))
                return script_error("$%04lX is unassigned", addr + i);
            script_poke(space, (uint16_t)(addr + i), (uint8_t)value);
            i++;
        }
        if (!i)
            return script_error("poke wants at least one byte value");
        return true;
    }

    if (!strcasecmp(cmd, "reply"))
    {
        bool on = true;
        char *word = script_word(&p);
        if (word && !strcasecmp(word, "off"))
            on = false;
        else if (word && strcasecmp(word, "on"))
            return script_error("reply wants on or off");
        script_replies = on;
        return true;
    }

    if (!strcasecmp(cmd, "dump"))
    {
        script_space_t space;
        long addr, count = 1;
        if (!script_address(&p, &space, &addr))
            return script_error("dump wants an address");
        if (space == SCRIPT_BUS)
            return script_error("bus: is only for poke");
        if (script_more(&p) && (!script_number(&p, &count) || count < 1))
            return script_error("dump wants a byte count");
        for (long i = 0; i < count && addr + i <= 0xFFFF; i++)
            if (script_unassigned(space, addr + i))
                return script_error("$%04lX is unassigned", addr + i);
        if (script_replies)
            printf("ok");
        for (long i = 0; i < count && addr + i <= 0xFFFF; i++)
            printf("%s%02X", i || script_replies ? " " : "",
                   script_peek(space, (uint16_t)(addr + i)));
        printf("\n");
        fflush(stdout);
        script_answered = script_replies;
        return true;
    }

    if (!strcasecmp(cmd, "cpu"))
    {
        char cpu[96];
        script_cpu(cpu, sizeof cpu);
        printf(script_replies ? "ok %s\n" : "%s\n", cpu);
        fflush(stdout);
        script_answered = script_replies;
        return true;
    }

    /* The canvas is checked against a remembered hash rather than a literal
     * one, because a hash written into a script is invalidated by every
     * unrelated rendering change. A driver that wants the number reads
     * `crc`. */
    if (!strcasecmp(cmd, "crc") || !strcasecmp(cmd, "mark") ||
        !strcasecmp(cmd, "expect-same") || !strcasecmp(cmd, "expect-changed"))
    {
        uint32_t crc;
        if (!vga_frame_crc(&crc))
            return script_error("no framebuffer to hash");
        if (!strcasecmp(cmd, "crc"))
        {
            printf(script_replies ? "ok %08X\n" : "%08X\n", crc);
            fflush(stdout);
            script_answered = script_replies;
            return true;
        }
        if (!strcasecmp(cmd, "mark"))
        {
            script_mark = crc;
            script_marked = true;
            return true;
        }
        if (!script_marked)
            return script_error("%s wants a mark first", cmd);
        bool same = crc == script_mark;
        if (same != !strcasecmp(cmd, "expect-same"))
            return script_error("the canvas %s since the mark", same ? "has not changed" : "changed");
        return true;
    }

    if (!strcasecmp(cmd, "shot"))
    {
        char path[SCRIPT_LINE_MAX]; /* a token cannot outrun its own line */
        if (!script_string(&p, path, sizeof path))
            return script_error("shot wants a quoted path");
        const uint32_t *fb = vga_get_framebuffer();
        if (!fb)
            return script_error("no framebuffer to write");
        int w, h;
        vga_canvas_size(&w, &h);
        if (!png_write(path, w, h, fb))
            return script_error("cannot write '%s'", path);
        return true;
    }

    if (!strcasecmp(cmd, "state"))
    {
        char *what = script_word(&p);
        if (!what || (strcasecmp(what, "save") && strcasecmp(what, "load")))
            return script_error("state wants save or load");
        bool saving = !strcasecmp(what, "save");
        char path[SCRIPT_LINE_MAX];
        if (!script_string(&p, path, sizeof path) || !path[0])
            return script_error("state %s wants a quoted path",
                                saving ? "save" : "load");
        const char *why = "";
        if (saving ? !state_save_file(path, &why) : !state_load_file(path, &why))
            return script_error("cannot %s '%s': %s", saving ? "save" : "load",
                                path, why);
        return true;
    }

    if (!strcasecmp(cmd, "seed"))
    {
        printf(script_replies ? "ok %u\n" : "%u\n", host_seed());
        fflush(stdout);
        script_answered = script_replies;
        return true;
    }

    /* The script is UTF-8. An installed ROM's path stays a host path in
     * UTF-8, while a name or a path that the machine resolves is converted to
     * OEM in place, which only ever shortens it. */
    if (!strcasecmp(cmd, "install"))
    {
        char path[SCRIPT_LINE_MAX];
        if (!script_string(&p, path, sizeof path) || !path[0])
            return script_error("install wants a quoted path");
        char *as = script_more(&p) ? script_word(&p) : NULL;
        if (as && *as == ':')
            as++;
        if (as)
            oem_from_utf8(as, as, strlen(as) + 1);
        if (!(as ? rom_alias_insert_as(path, as) : rom_alias_insert(path)))
            return script_error("cannot install '%s'", path);
        return true;
    }

    if (!strcasecmp(cmd, "remove"))
    {
        char *name = script_word(&p);
        if (!name || !*name)
            return script_error("remove wants an installed name");
        oem_from_utf8(name, name, strlen(name) + 1);
        if (!rom_alias_remove(name))
            return script_error("nothing installed as '%s'", name);
        return true;
    }

    if (!strcasecmp(cmd, "load"))
    {
        char path[SCRIPT_LINE_MAX];
        if (!script_string(&p, path, sizeof path) || !path[0])
            return script_error("load wants a quoted path");
        oem_from_utf8(path, path, sizeof path);
        /* A running program would have its memory written out from under it,
         * so a load happens only between programs. */
        if (sys_active())
            return script_error("load wants a stopped machine");
        vtkeys_paste_cancel();
        if (!proc_boot(path, 0, NULL, PROC_UNCHAIN))
            return script_error("cannot load '%s'", path);
        sys_commit();
        return true;
    }

    if (!strcasecmp(cmd, "sys"))
    {
        char *what = script_word(&p);
        if (!what)
            return script_error("sys wants run, stop or break");
        if (!strcasecmp(what, "run"))
            sys_run();
        else if (!strcasecmp(what, "stop"))
            sys_stop();
        else if (!strcasecmp(what, "break"))
            sys_break_request();
        else
            return script_error("sys wants run, stop or break");
        sys_commit();
        return true;
    }

    return script_error("unknown command '%s'", cmd);
}

static bool script_spend(void)
{
    if (!script_budget)
        return false;
    script_budget--;
    return true;
}

/* An exhausted budget is how a 'run' finishes; on every other wait the same
 * counter is a timeout that fails the run rather than hanging it. */
static bool script_settle(void)
{
    switch (script_wait)
    {
    case SCRIPT_IDLE:
        return true;
    case SCRIPT_FRAMES:
        if (script_spend())
            return false;
        break;
    case SCRIPT_TEXT:
    {
        char *found = script_find(script_wait_cap, script_needle, script_needle_len);
        if (found)
        {
            script_cap_take(script_wait_cap, found + script_needle_len);
            break;
        }
        if (script_spend())
            return false;
        char shown[4 * sizeof script_needle];
        script_printable(shown, sizeof shown, script_needle, script_needle_len);
        return script_error("timed out waiting for \"%s\"%s", shown,
                            script_on(script_wait_cap));
    }
    case SCRIPT_BYTE:
    {
        uint8_t got = script_peek(script_addr_space, (uint16_t)script_addr);
        if (got == script_addr_want)
            break;
        if (script_spend())
            return false;
        return script_error("timed out waiting for %s:$%04lX to read $%02X; it is $%02X",
                            script_addr_space == SCRIPT_XRAM ? "xram" : "ram",
                            script_addr, script_addr_want, got);
    }
    case SCRIPT_TYPING:
        if (!vtkeys_paste_busy())
            break;
        if (script_spend())
            return false;
        return script_error("timed out typing; the program is not reading its input");
    case SCRIPT_EXIT:
        if (proc_exited())
        {
            int code = proc_get_exit_code();
            if (code != script_exit_want)
                return script_error("exit code %d, expected %d", code, script_exit_want);
            break;
        }
        if (script_spend())
            return false;
        return script_error("timed out waiting for the program to exit");
    }
    script_wait = SCRIPT_IDLE;
    return true;
}

void script_task(void)
{
    char line[SCRIPT_LINE_MAX];
    while (script_run && script_settle())
    {
        /* Settling is what says the last command finished, so this is where it
         * is answered: after the frames it asked for, not when it parsed. */
        if (script_pending)
        {
            if (!script_answered)
                script_reply("ok");
            script_pending = script_answered = false;
        }
        if (!fgets(line, sizeof line, script_file))
        {
            script_run = false;
            return;
        }
        script_line_no++;
        script_pending = true;
        if (!script_command(line))
            return;
    }
}

void script_usage(FILE *out)
{
    fprintf(out,
            "\nscript commands (one per line, # comments, text always quoted;\n"
            "a failed check names the line and the cpu, then exits 1):\n"
            "  run [frames]              let frames elapse (default 1)\n"
            "  wait [stdout|stderr] \"text\" [frames]\n"
            "                                      run until the program writes it\n"
            "                                      (default stdout, 600 frames)\n"
            "  wait [xram:]<addr> <byte> [frames]  run until that byte reads that\n"
            "  type \"text\" [frames]      type it (\\r = Enter, \\t = Tab)\n"
            "  key <key>[+ctrl][+shift][+alt]    send what a terminal sends\n"
            "  press/release <key>...    the direct HID bitmap, by name or 0xNN\n"
            "  lock num|caps|scroll      toggle a lock LED\n"
            "  pad <n> connect [western|eastern|playstation] [sticks] | disconnect\n"
            "  pad <n> press|release <button>...   a b c x y z l1 r1 l2 r2 l3 r3\n"
            "                                      select start home up down left right\n"
            "  pad <n> stick <lx> <ly> <rx> <ry>   -128..127\n"
            "  pad <n> trigger <lt> <rt>           0..255\n"
            "  mouse move <dx> <dy> | wheel <n> [pan] | buttons <mask>\n"
            "  tablet at <x> <y> [buttons] | touch <x>,<y>... | wheel <n> [pan] | clear\n"
            "  expect [stdout|stderr] \"text\"       check the stream since its last\n"
            "  expect-not [stdout|stderr] \"text\"   match (default stdout)\n"
            "  expect-exit <code> [frames]         run until it exits, check the code\n"
            "  peek [xram:]<addr> <byte>...        compare memory\n"
            "  poke [xram:|bus:]<addr> <byte>...   write memory; bus: stores as the 6502\n"
            "  dump [xram:]<addr> [count]          print memory as hex\n"
            "  cpu                                 PC A X Y SP P, then OP BUSY RESB\n"
            "  crc                                 the canvas as a CRC-32\n"
            "  mark, expect-same, expect-changed   the canvas against a remembered one\n"
            "  shot \"file.png\"           write the canvas\n"
            "  state save \"file\" / state load \"file\"   the machine, written down\n"
            "  seed                      print this run's seed\n"
            "  install \"path\" [NAME]     put a file on the null drive as :NAME\n"
            "  remove <NAME>             take it back off\n"
            "  load \"path\"               boot a program on a stopped machine\n"
            "  sys run|stop|break        start, stop, or interrupt the machine\n"
            "  reply [on|off]            answer every command on stdout, for a\n"
            "                            driver on the other end of a pipe\n");
}

bool script_load(const char *path)
{
    if (!strcmp(path, "-"))
    {
        script_file = stdin;
        script_path = "<stdin>";
    }
    else
    {
        script_file = os_fopen(path, "r");
        if (!script_file)
        {
            fprintf(stderr, "rp6502-emu: cannot open script '%s'\n", path);
            return false;
        }
        script_path = path;
    }
    com_set_std_tap(script_std_tap);
    script_line_no = 0;
    script_wait = SCRIPT_IDLE;
    script_budget = 0;
    for (int i = 0; i < 2; i++)
    {
        script_cap[i].len = 0;
        script_cap[i].buf[0] = 0;
    }
    script_stderr_midline = false;
    script_marked = false;
    script_fail = false;
    script_replies = false;
    script_answered = false;
    script_pending = false;
    script_run = true;
    return true;
}

bool script_loaded(void)
{
    return script_file != NULL;
}

bool script_running(void)
{
    return script_run;
}

int script_exit_code(void)
{
    return script_fail ? 1 : 0;
}
