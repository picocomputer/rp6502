/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "host/sokol/cli/script.h"
#include "host/sokol/cli/png.h"
#include "core/api/proc.h"
#include "core/hid/keyboard.h"
#include "core/hid/usage.h"
#include "core/hid/vtkeys.h"
#include "core/hid/mouse.h"
#include "core/hid/gamepad.h"
#include "core/hid/tablet.h"
#include "core/com/com.h"
#include "core/wdc/resb.h"
#include "core/wdc/sram.h"
#include "core/sys/xram.h"
#include "host/host.h"
#include "core/vga/vga_emu.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define SCRIPT_LINE_MAX 1024
#define SCRIPT_CAP_SIZE 65536
#define SCRIPT_CAP_KEEP 4096   /* console tail kept when the capture fills */
#define SCRIPT_WAIT_FRAMES 600 /* default budget for a command that blocks */

/* What the script is waiting for before its next command runs. */
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
static const char *script_path = "<script>"; /* named before a file, for script_command */
static int script_line_no;
static bool script_run, script_fail;

static script_wait_t script_wait;
/* Frames still owed to the pending wait. script_task hands control back only when
 * one is due, so `run 600` costs exactly 600 frames and a budget expires on the
 * frame it names — neither depends on how often anything else polls. */
static unsigned long script_budget;
static char script_needle[256];
static int script_exit_want;

/* Where a wait on memory is looking. The byte is read at the frame boundary,
 * which is why the program on the other end holds its answer until the script
 * acknowledges it rather than publishing it for a cycle and moving on. */
static uint8_t *script_addr_base;
static long script_addr;
static uint8_t script_addr_want;

/* The canvas as `mark` last saw it. A test of a graphic is usually "this moved"
 * or "this held still", which a remembered hash answers without a literal one
 * that every unrelated rendering change would invalidate. */
static uint32_t script_mark;
static bool script_marked;

/* Every player's report, assembled here and handed to gamepad_host_report — the
 * same shape the web and Android hosts keep, so a scripted gamepad reaches XRAM
 * through the code a real one does. */
static struct
{
    bool connected;
    bool sticks;
    uint8_t type;
    uint8_t dpad, button0, button1;
    int lx, ly, rx, ry, lt, rt;
} script_gamepad[4];

/* ------------------------------------------------------------------ */
/* Console capture                                                     */
/* ------------------------------------------------------------------ */

static char script_cap[SCRIPT_CAP_SIZE];
static size_t script_cap_len;

static void script_tap(const char *buf, int len)
{
    for (int i = 0; i < len; i++)
    {
        if (script_cap_len == sizeof script_cap - 1)
        {
            /* Only output nothing has matched yet gets this far; keep the tail
             * so a needle straddling the discard still has somewhere to land. */
            memmove(script_cap, script_cap + script_cap_len - SCRIPT_CAP_KEEP, SCRIPT_CAP_KEEP);
            script_cap_len = SCRIPT_CAP_KEEP;
        }
        script_cap[script_cap_len++] = buf[i];
    }
    script_cap[script_cap_len] = 0;
}

/* Take the console up to and including a match, leaving the rest to be matched
 * by whatever the script checks next — two needles in one burst of output are
 * two checks, not a race. */
static void script_cap_take(const char *through)
{
    size_t used = (size_t)(through - script_cap);
    memmove(script_cap, script_cap + used, script_cap_len - used + 1);
    script_cap_len -= used;
}

/* ------------------------------------------------------------------ */
/* Parsing                                                             */
/* ------------------------------------------------------------------ */

/* What the console actually held. The capture is the wire: it carries the line
 * editor's escapes and the echo of anything the script typed, so a needle that
 * spans a cursor move never matches and the only way to see why is to look. */
static void script_show_capture(void)
{
    size_t from = script_cap_len > 200 ? script_cap_len - 200 : 0;
    fputs("  console: ", stderr);
    for (size_t i = from; i < script_cap_len; i++)
    {
        unsigned char c = (unsigned char)script_cap[i];
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

static bool script_error(const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "rp6502-emu: %s:%d: %s\n", script_path, script_line_no, msg);
    script_reply("fail %s", msg);
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

/* The next whitespace-delimited word, terminated in place. NULL at end of line. */
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

/* A double-quoted argument with \n \r \t \\ \" escapes. Text is always quoted
 * so a trailing space or an empty string means what it says. */
static bool script_string(char **p, char *out, size_t outsz)
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

/* An address, in RAM unless it says xram:. */
static bool script_address(char **p, uint8_t **base, long *addr)
{
    char *word = script_word(p);
    if (!word)
        return false;
    *base = sram;
    if (!strncasecmp(word, "xram:", 5))
        *base = (uint8_t *)xram, word += 5;
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

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static void script_gamepad_publish(int player)
{
    gamepad_connect(player, true, script_gamepad[player].type, script_gamepad[player].sticks);
    gamepad_host_report(player, script_gamepad[player].dpad, script_gamepad[player].button0,
                        script_gamepad[player].button1, script_gamepad[player].lx, script_gamepad[player].ly,
                    script_gamepad[player].rx, script_gamepad[player].ry, script_gamepad[player].lt,
                    script_gamepad[player].rt);
}

static bool script_canvas_crc(uint32_t *out)
{
    const uint32_t *fb = vga_get_framebuffer();
    if (!fb)
        return false;
    int w, h;
    vga_canvas_size(&w, &h);
    *out = host_crc32(0, fb, (size_t)w * h * 4);
    return true;
}

/* The button names a script writes, and the buttons core/hid knows. Only a
 * script needs the names -- a device sends bits. */
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
        /* What a real host would know about the gamepad it found, if anything. */
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
            script_gamepad_publish((int)player); /* the claim lands now, not on first press */
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
        mouse_host_move((float)dx, (float)dy);
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
        tablet_host_pointer((int)x, (int)y, (uint8_t)buttons);
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

/* ------------------------------------------------------------------ */
/* Key names                                                           */
/* ------------------------------------------------------------------ */

/* Only the script language takes a key as text; every other caller already
 * holds a usage. So the table lives here rather than in core, where libretro
 * and android were linking it and never asking. */

/* The keys with a name instead of a character. Letters, digits, function keys
 * and the keypad are computed below rather than listed. key is -1 where the key
 * has no xterm sequence of its own. */
static const struct
{
    const char *name;
    uint8_t hid;
} script_named[] = {
    {"enter", 0x28},
    {"escape", 0x29},
    {"backspace", 0x2A},
    {"tab", 0x2B},
    {"up", 0x52},
    {"down", 0x51},
    {"left", 0x50},
    {"right", 0x4F},
    {"home", 0x4A},
    {"end", 0x4D},
    {"insert", 0x49},
    {"delete", 0x4C},
    {"pageup", 0x4B},
    {"pagedown", 0x4E},
    {"kpenter", 0x58},
    {"space", 0x2C},
    {"minus", 0x2D},
    {"equal", 0x2E},
    {"leftbracket", 0x2F},
    {"rightbracket", 0x30},
    {"backslash", 0x31},
    {"semicolon", 0x33},
    {"apostrophe", 0x34},
    {"grave", 0x35},
    {"comma", 0x36},
    {"period", 0x37},
    {"slash", 0x38},
    {"capslock", 0x39},
    {"printscreen", 0x46},
    {"scrolllock", 0x47},
    {"pause", 0x48},
    {"numlock", 0x53},
    {"menu", 0x65},
    {"kpdivide", 0x54},
    {"kpmultiply", 0x55},
    {"kpsubtract", 0x56},
    {"kpadd", 0x57},
    {"kpdecimal", 0x63},
    {"kpequal", 0x67},
    {"lctrl", 0xE0},
    {"lshift", 0xE1},
    {"lalt", 0xE2},
    {"lsuper", 0xE3},
    {"rctrl", 0xE4},
    {"rshift", 0xE5},
    {"ralt", 0xE6},
    {"rsuper", 0xE7},
};

/* "f1".."f12" -> 1..12, else 0. */
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

static uint8_t script_hid_from_name(const char *name)
{
    if (!name || !name[0])
        return 0;
    if (!name[1]) /* a bare letter or digit is its own name */
    {
        char c = name[0];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c + 32);
        if (c >= 'a' && c <= 'z')
            return (uint8_t)(0x04 + c - 'a');
        if (c >= '1' && c <= '9')
            return (uint8_t)(0x1E + c - '1');
        if (c == '0')
            return 0x27;
        return 0;
    }
    if ((name[0] == 'k' || name[0] == 'K') && (name[1] == 'p' || name[1] == 'P') &&
        name[2] >= '0' && name[2] <= '9' && !name[3])
        return name[2] == '0' ? 0x62 : (uint8_t)(0x59 + name[2] - '1');
    int f = script_fkey_num(name);
    if (f)
        return (uint8_t)(0x3A + f - 1);
    for (size_t i = 0; i < sizeof script_named / sizeof script_named[0]; i++)
        if (!strcasecmp(name, script_named[i].name))
            return script_named[i].hid;
    return 0;
}

bool script_command(const char *line)
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
        /* Text or a byte in memory. script_string does not consume the word when
         * it finds no quote, so trying it first costs nothing and the two
         * grammars cannot be confused for each other. */
        char text[sizeof script_needle];
        if (script_string(&p, text, sizeof text))
        {
            if (!text[0])
                return script_error("wait wants a quoted string");
            snprintf(script_needle, sizeof script_needle, "%s", text);
            script_wait = SCRIPT_TEXT;
        }
        else
        {
            long want;
            if (!script_address(&p, &script_addr_base, &script_addr) ||
                !script_number(&p, &want) || want < 0 || want > 0xFF)
                return script_error("wait wants a quoted string, or an address "
                                 "and the byte to wait for");
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
        char text[256];
        if (!script_string(&p, text, sizeof text) || !text[0])
            return script_error("%s wants a quoted string", cmd);
        char *found = strstr(script_cap, text);
        if (want && !found)
            return script_error("expected \"%s\"", text);
        if (!want && found)
            return script_error("did not expect \"%s\"", text);
        if (found)
            script_cap_take(found + strlen(text));
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
        /* Blocks until the ring has it all, so back-to-back type commands
         * cannot replace each other mid-drip. */
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
        uint8_t hid = script_hid_from_name(name);
        if (!hid || !vtkeys_key(hid, ctrl, shift, alt))
            return script_error("'%s' has no key sequence", name);
        return true;
    }

    if (!strcasecmp(cmd, "press") || !strcasecmp(cmd, "release"))
    {
        bool down = !strcasecmp(cmd, "press");
        char *name;
        int count = 0;
        while ((name = script_word(&p)) != NULL)
        {
            uint8_t hid = script_hid_from_name(name);
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
        uint8_t *base;
        long addr, want;
        if (!script_address(&p, &base, &addr))
            return script_error("peek wants an address");
        int i = 0;
        while (script_more(&p))
        {
            if (!script_number(&p, &want) || want < 0 || want > 255)
                return script_error("peek wants byte values 0..255");
            if (addr + i > 0xFFFF)
                return script_error("peek runs past the end of memory");
            if (base[addr + i] != (uint8_t)want)
                return script_error("$%04lX+%d is $%02X, expected $%02lX",
                                    addr, i, base[addr + i], want);
            i++;
        }
        if (!i)
            return script_error("peek wants at least one byte value");
        return true;
    }

    /* Writes land in sram[]/xram[] only. The VIA and the RIA answer the bus, not
     * memory, so a poke changes what the program reads and triggers nothing. */
    if (!strcasecmp(cmd, "poke"))
    {
        uint8_t *base;
        long addr, value;
        if (!script_address(&p, &base, &addr))
            return script_error("poke wants an address");
        int i = 0;
        while (script_more(&p))
        {
            if (!script_number(&p, &value) || value < 0 || value > 255)
                return script_error("poke wants byte values 0..255");
            if (addr + i > 0xFFFF)
                return script_error("poke runs past the end of memory");
            base[addr + i] = (uint8_t)value;
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
        uint8_t *base;
        long addr, count = 1;
        if (!script_address(&p, &base, &addr))
            return script_error("dump wants an address");
        if (script_more(&p) && (!script_number(&p, &count) || count < 1))
            return script_error("dump wants a byte count");
        if (script_replies)
            printf("ok");
        for (long i = 0; i < count && addr + i <= 0xFFFF; i++)
            printf("%s%02X", i || script_replies ? " " : "", base[addr + i]);
        printf("\n");
        fflush(stdout);
        script_answered = script_replies;
        return true;
    }

    /* The canvas is checked against a remembered hash, never a literal one:
     * a hash written into a script is invalidated by every unrelated
     * rendering change. A driver that wants the number reads `crc`. */
    if (!strcasecmp(cmd, "crc") || !strcasecmp(cmd, "mark") ||
        !strcasecmp(cmd, "expect-same") || !strcasecmp(cmd, "expect-changed"))
    {
        uint32_t crc;
        if (!script_canvas_crc(&crc))
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

    return script_error("unknown command '%s'", cmd);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/* Spend one of the pending wait's frames. False once the budget is gone. */
static bool script_spend(void)
{
    if (!script_budget)
        return false;
    script_budget--;
    return true;
}

/* True once whatever the script is waiting for has happened. A budget that runs
 * out fails the run here rather than hanging it. */
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
        char *found = strstr(script_cap, script_needle);
        if (found)
        {
            script_cap_take(found + strlen(script_needle));
            break;
        }
    }
        if (script_spend())
            return false;
        script_show_capture();
        return script_error("timed out waiting for \"%s\"", script_needle);
    case SCRIPT_BYTE:
        if (script_addr_base[script_addr] == script_addr_want)
            break;
        if (script_spend())
            return false;
        return script_error("timed out waiting for %s:$%04lX to read $%02X; it is $%02X",
                            script_addr_base == xram ? "xram" : "ram", script_addr,
                         script_addr_want, script_addr_base[script_addr]);
    case SCRIPT_TYPING:
        if (!vtkeys_paste_busy())
            break;
        if (script_spend())
            return false;
        return script_error("timed out typing; the program is not reading its input");
    case SCRIPT_EXIT:
        if (!resb_running())
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

/* Run the script forward until it owes the machine a frame, which is the only
 * reason it hands control back. The caller runs exactly one frame per return,
 * so a wait costs the frames it asked for and not one more. */
void script_task(void)
{
    char line[SCRIPT_LINE_MAX];
    while (script_run && script_settle())
    {
        /* Settling is what says the last command is done, so this is where it
         * is answered — after the frames it asked for, not when it parsed. */
        if (script_pending)
        {
            if (!script_answered)
                script_reply("ok");
            script_pending = script_answered = false;
        }
        if (!fgets(line, sizeof line, script_file))
        {
            script_run = false; /* end of script */
            return;
        }
        script_line_no++;
        script_pending = true;
        if (!script_command(line))
            return; /* script_error ended the run */
    }
}

/* The verbs, printed beside where they are implemented. A second copy in
 * cli.c is a second thing to remember, and the one that gets forgotten is
 * the copy nobody is reading while they change the parser. */
void script_usage(FILE *out)
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
            "  gamepad <n> connect [western|eastern|playstation] [sticks] | disconnect\n"
            "  gamepad <n> press|release <button>...   a b c x y z l1 r1 l2 r2 l3 r3\n"
            "                                      select start home up down left right\n"
            "  gamepad <n> stick <lx> <ly> <rx> <ry>   -128..127\n"
            "  gamepad <n> trigger <lt> <rt>           0..255\n"
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

bool script_load(const char *path)
{
    if (!strcmp(path, "-"))
    {
        script_file = stdin;
        script_path = "<stdin>";
    }
    else
    {
        script_file = fopen(path, "r");
        if (!script_file)
        {
            fprintf(stderr, "rp6502-emu: cannot open script '%s'\n", path);
            return false;
        }
        script_path = path;
    }
    com_set_tx_tap(script_tap);
    /* Arm a clean run: a load inherits nothing from a script that ran before it,
     * not a half-finished wait, not console text nobody matched, not a verdict. */
    script_line_no = 0;
    script_wait = SCRIPT_IDLE;
    script_budget = 0;
    script_cap_len = 0;
    script_cap[0] = 0;
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
