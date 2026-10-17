/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "host/sokol/cli/script.h"
#include "host/sokol/cli/expr.h"
#include "host/sokol/cli/png.h"
#include "host/sokol/cli/state.h"
#include "core/api/api.h"
#include "core/api/std.h"
#include "core/rom/rom.h"
#include "core/str/oem.h"
#include "core/sys/sys.h"
#include "host/host.h"
#include "core/api/arg.h"
#include "core/api/proc.h"
#include "core/sys/proc.h"
#include "core/sys/random.h"
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
#include "core/aud/mix.h"
#include "osal/os.h"
#include "chips/chips/w65c02.h"
#include <ctype.h>
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
    SCRIPT_EOF,
    SCRIPT_WAV,
} script_wait_t;

static FILE *script_file;
static const char *script_path = "<script>"; /* until a file is opened */
static int script_line_no;
static bool script_run, script_fail;

static script_wait_t script_wait;
static unsigned long script_budget;
static char script_needle[256];
static size_t script_needle_len;

/* The seeds and the memory fill that status prints. The RNG seed is the last
 * one that seed set, or host_seed() until then. */
static uint32_t script_memseed;
static uint8_t script_memfill;
static bool script_memfill_set;
static uint32_t script_rng_seed;
static bool script_rng_set;

/* A machine that sys stop or sys break stopped has no exit code of its own,
 * so EXIT and the exit checks wait for the next load or sys run. */
static bool script_halted;

/* Where an address points. Without a prefix, or with ram:, it is the 6502's
 * address space. bus: is the same space, written the way the 6502 writes it.
 * xbus: is XRAM, written the way the RIA writes it for the 6502. */
typedef enum
{
    SCRIPT_CPU,
    SCRIPT_XRAM,
    SCRIPT_BUS,
    SCRIPT_XBUS,
} script_space_t;

/* A `wait` on memory looks here. script_settle samples the byte once a
 * frame, so a program that publishes a value for a few cycles and moves on can
 * be missed. */
static script_space_t script_addr_space;
static long script_addr;
static uint8_t script_addr_want;

static uint32_t script_mark;
static bool script_marked;

/* Sound is made at mix.c's own default rate, one frame of it per frame. */
#define SCRIPT_AUD_RATE 48000
#define SCRIPT_AUD_FRAMES (SCRIPT_AUD_RATE / VGA_HZ)

/* A wav larger than this would not fit the 32-bit sizes in its header. */
#define SCRIPT_WAV_MAX_FRAMES 1000000

/* The file `wav` is writing, NULL when it is not. */
static FILE *script_wav;

static struct
{
    bool connected;
    bool sticks;
    uint8_t type;
    uint8_t dpad, button0, button1;
    int lx, ly, rx, ry, lt, rt;
} script_gamepad[4];

/* What no check has used yet, converted from the code page to UTF-8 as it was
 * written, so that it compares with the script's own text. stdout and stderr
 * are the bytes the program wrote. console is what reaches the screen: those
 * bytes with the CR the console puts before each newline, the line editor's
 * echo of typed text, and the system's own messages. */
typedef struct
{
    char buf[SCRIPT_CAP_SIZE];
    size_t len;
} script_cap_t;

static script_cap_t script_cap[3];
#define SCRIPT_STDOUT (&script_cap[0])
#define SCRIPT_STDERR (&script_cap[1])
#define SCRIPT_CONSOLE (&script_cap[2])

/* The text a `wait` is looking in. */
static script_cap_t *script_wait_cap;

/* The program's stderr reaches the host's as it is written. This says whether
 * it stopped partway through a line. */
static bool script_stderr_midline;

static void script_cap_put(script_cap_t *cap, const char *buf, int len)
{
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

static void script_std_tap(int fd, const char *buf, int len)
{
    if (fd != 1 && fd != 2)
        return;
    if (fd == 2 && len)
        script_stderr_midline = buf[len - 1] != '\n';
    script_cap_put(&script_cap[fd - 1], buf, len);
}

static void script_console_tap(const char *buf, int len)
{
    script_cap_put(SCRIPT_CONSOLE, buf, len);
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

static const char *const script_cap_names[] = {"stdout", "stderr", "console"};

/* An optional stream before a string. Without one it is stdout. */
static script_cap_t *script_stream(char **p)
{
    char *s = *p;
    while (*s == ' ' || *s == '\t')
        s++;
    size_t n = strcspn(s, " \t");
    for (int i = 0; i < 3; i++)
        if (n == strlen(script_cap_names[i]) && !strncasecmp(s, script_cap_names[i], n))
        {
            *p = s + n;
            return &script_cap[i];
        }
    return SCRIPT_STDOUT;
}

/* Messages name the stream only when it is not the default. */
static const char *script_on(const script_cap_t *cap)
{
    if (cap == SCRIPT_STDERR)
        return " on stderr";
    if (cap == SCRIPT_CONSOLE)
        return " on console";
    return "";
}

static uint32_t script_rng(void)
{
    return script_rng_set ? script_rng_seed : host_seed();
}

/* The program exited, as opposed to a stop that the script asked for. */
static bool script_exited(void)
{
    return proc_exited() && !script_halted;
}

/* The status line. ROM is last, so its path needs no quotes. */
#define SCRIPT_STATUS_SIZE 1024
static void script_status(char *buf, size_t size)
{
    w65c02_t *c = cpu_chip();
    char exit_code[16] = "", memory[24], rom[768];
    if (script_exited())
        snprintf(exit_code, sizeof exit_code, " EXIT=%d", proc_get_exit_code());
    if (script_memfill_set)
        snprintf(memory, sizeof memory, "MEMFILL=$%02X", script_memfill);
    else
        snprintf(memory, sizeof memory, "MEMSEED=%u", (unsigned)script_memseed);
    /* After a stop, argv[0] still names the ROM that ran last. */
    const char *running = proc_running();
    if (!running[0] && arg_index(0))
        running = arg_index(0);
    oem_to_utf8(running, rom, sizeof rom);
    snprintf(buf, size,
             "PC=$%04X A=$%02X X=$%02X Y=$%02X SP=$%02X P=$%02X OP=$%02X BUSY=$%02X "
             "RESB=%d%s SEED=%u %s ROM=%s",
             w65c02_pc(c), w65c02_a(c), w65c02_x(c), w65c02_y(c), w65c02_s(c),
             w65c02_p(c), API_OP, REGS(0xFFF2), sys_running() ? 1 : 0, exit_code,
             (unsigned)script_rng(), memory, rom);
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

/* Every failure is followed by a status line, because the run ends here and a
 * driver can't ask afterwards. */
static bool script_error(const char *fmt, ...)
{
    /* Only the first error on a line is reported, so a command's usage
     * message never follows the error from its expression. */
    if (script_fail)
        return false;
    char msg[1024], text[4096], status[SCRIPT_STATUS_SIZE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    script_printable(text, sizeof text, msg, strlen(msg));
    script_status(status, sizeof status);
    script_reply("fail %s", text);
    script_reply("%s", status);
    /* The program's stdout that no check has used goes out after the failure,
     * up to EOF, in the UTF-8 the checks search. Its stderr is on stderr
     * already. */
    fwrite(SCRIPT_STDOUT->buf, 1, SCRIPT_STDOUT->len, stdout);
    fflush(stdout);
    /* The message goes last, on a line of its own, so a program that clears
     * or resets the screen can't wipe it. */
    if ((SCRIPT_STDOUT->len && SCRIPT_STDOUT->buf[SCRIPT_STDOUT->len - 1] != '\n') ||
        script_stderr_midline)
        fputc('\n', stderr);
    fprintf(stderr, "rp6502-emu: %s:%d: %s\nrp6502-emu: %s\n", script_path, script_line_no,
            text, status);
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

/* The first of the stop characters outside parentheses, or the end. */
static char *script_outside(char *s, const char *stop)
{
    int depth = 0;
    for (; *s && (depth || !strchr(stop, *s)); s++)
        if (*s == '(')
            depth++;
        else if (*s == ')' && depth)
            depth--;
    return s;
}

/* A space inside parentheses is part of the word, so (base + 4) is one. */
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
    s = script_outside(s, " \t");
    if (*s)
        *s++ = 0;
    *p = s;
    return word;
}

/* Takes the next word when it is the keyword. The line is left as it was
 * when it is not, so another grammar can read it. */
static bool script_keyword(char **p, const char *keyword)
{
    char *s = *p;
    while (*s == ' ' || *s == '\t')
        s++;
    size_t n = strlen(keyword);
    if (strncasecmp(s, keyword, n) || (s[n] && s[n] != ' ' && s[n] != '\t'))
        return false;
    *p = s + n;
    return true;
}

/* A command fails on a word after its last argument, as the monitor does. */
static bool script_end(char *p)
{
    char *word = script_word(&p);
    if (word)
        return script_error("unexpected '%s'; spaces separate arguments", word);
    return true;
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

/* A name the script can use, from the .sym beside the ROM or set by the
 * script itself. */
typedef struct
{
    char *name;
    int64_t value;
} script_name_t;

typedef struct
{
    script_name_t *at;
    size_t count;
} script_names_t;

static script_names_t script_syms, script_vars;

static void script_names_clear(script_names_t *names)
{
    for (size_t i = 0; i < names->count; i++)
        free(names->at[i].name);
    free(names->at);
    names->at = NULL;
    names->count = 0;
}

static bool script_names_add(script_names_t *names, const char *name, size_t len,
                             int64_t value)
{
    script_name_t *at = realloc(names->at, (names->count + 1) * sizeof *at);
    if (!at)
        return false;
    names->at = at;
    char *copy = malloc(len + 1);
    if (!copy)
        return false;
    memcpy(copy, name, len);
    copy[len] = 0;
    at[names->count].name = copy;
    at[names->count].value = value;
    names->count++;
    return true;
}

/* How many entries have the name, with the last of them in found. */
static int script_names_count(script_names_t *names, const char *name, size_t len,
                              script_name_t **found)
{
    int n = 0;
    for (size_t i = 0; i < names->count; i++)
        if (!strncmp(names->at[i].name, name, len) && !names->at[i].name[len])
        {
            *found = &names->at[i];
            n++;
        }
    return n;
}

/* A name on two .sym lines, or in both lists after a load, is ambiguous.
 * Names are case-sensitive, as in Python. */
static expr_status_t script_lookup(const char *name, size_t len, int64_t *value)
{
    script_name_t *found = NULL;
    int n = script_names_count(&script_syms, name, len, &found) +
            script_names_count(&script_vars, name, len, &found);
    if (n > 1)
        return EXPR_AMBIGUOUS;
    if (!n)
        return EXPR_UNKNOWN;
    *value = found->value;
    return EXPR_OK;
}

/* The end of the name at s, or s when no name starts there. Names are as in
 * Python, with . allowed after the first character. */
static char *script_name_end(char *s)
{
    if (isalpha((unsigned char)*s) || *s == '_')
        while (isalnum((unsigned char)*s) || *s == '_' || *s == '.')
            s++;
    return s;
}

/* Loads the .sym beside a ROM: the same path with a .sym extension, added
 * when it has none. No file means no names. False, with why filled in, when
 * a line is not a name and a whole number. */
static bool script_symbols(const char *rom, char *why, size_t size)
{
    script_names_clear(&script_syms);
    size_t len = strlen(rom);
    const char *dot = strrchr(rom, '.');
    if (dot && !strpbrk(dot, "/\\"))
        len = (size_t)(dot - rom);
    char *path = malloc(len + 5);
    if (!path)
        return snprintf(why, size, "out of memory"), false;
    memcpy(path, rom, len);
    memcpy(path + len, ".sym", 5);
    FILE *f = os_fopen(path, "r");
    char line[SCRIPT_LINE_MAX];
    int n = 0;
    bool ok = true;
    while (f && ok && fgets(line, sizeof line, f))
    {
        n++;
        line[strcspn(line, "\r\n")] = 0;
        char *value = strchr(line, ' ');
        int64_t v;
        ok = value && value > line && expr_int(value + 1, strlen(value + 1), &v) &&
             script_names_add(&script_syms, line, (size_t)(value - line), v);
    }
    if (f)
        fclose(f);
    if (!ok)
        snprintf(why, size, "cannot read '%s' line %d", path, n);
    free(path);
    return ok;
}

/* Works out an expression, failing the line with its error. A message shows
 * the expression without one pair of parentheses around all of it. */
static bool script_expr(const char *text, int64_t *out)
{
    int len = (int)strlen(text), depth = 0;
    bool outer = text[0] == '(';
    for (int i = 0; i < len; i++)
    {
        if (text[i] == '(')
            depth++;
        else if (text[i] == ')' && depth)
        {
            depth--;
            if (!depth && i + 1 < len)
                outer = false;
        }
    }
    if (depth)
        return script_error("missing ) in '%s'", text);
    const char *shown = outer ? text + 1 : text;
    int n = outer ? len - 2 : len;
    const char *name = NULL;
    size_t name_len = 0;
    expr_status_t st = expr_eval(text, (size_t)len, script_lookup, out, &name, &name_len);
    switch (st)
    {
    case EXPR_OK:
        return true;
    case EXPR_BAD:
        return script_error("bad expression '%.*s'", n, shown);
    case EXPR_UNKNOWN:
        return script_error("unknown name '%.*s'", (int)name_len, name);
    case EXPR_AMBIGUOUS:
        return script_error("ambiguous name '%.*s'", (int)name_len, name);
    case EXPR_ZERO:
        return script_error("%.*s divides by zero", n, shown);
    case EXPR_OVERFLOW:
        return script_error("%.*s overflows 64 bits", n, shown);
    case EXPR_SHIFT:
        return script_error("%.*s shifts by a negative count", n, shown);
    case EXPR_FLOAT:
        break;
    case EXPR_DIVIDE:
        return script_error("%.*s: floats are not allowed, use //", n, shown);
    }
    return script_error("%.*s: floats are not allowed", n, shown);
}

/* Every number is an expression. */
static bool script_value(char *word, long *out)
{
    int64_t v;
    if (!script_expr(word, &v) || (long)v != v)
        return false;
    *out = (long)v;
    return true;
}

static bool script_number(char **p, long *out)
{
    char *word = script_word(p);
    return word && script_value(word, out);
}

static bool script_address(char **p, script_space_t *space, long *addr)
{
    char *word = script_word(p);
    if (!word)
        return false;
    *space = SCRIPT_CPU;
    if (!strncasecmp(word, "xram:", 5))
        *space = SCRIPT_XRAM, word += 5;
    else if (!strncasecmp(word, "xbus:", 5))
        *space = SCRIPT_XBUS, word += 5;
    else if (!strncasecmp(word, "bus:", 4))
        *space = SCRIPT_BUS, word += 4;
    else if (!strncasecmp(word, "ram:", 4))
        word += 4;
    long value;
    if (!script_value(word, &value) || value < 0 || value > 0xFFFF)
        return false;
    *addr = value;
    return true;
}

/* let name = expr sets a variable. A # that starts a word begins a comment,
 * as on any other line, and ends the expression. */
static bool script_assign(const char *name, size_t len, char *expr)
{
    while (*expr == ' ' || *expr == '\t')
        expr++;
    char *end = expr;
    while (*end && *end != '#')
    {
        end = script_outside(end, " \t");
        while (*end == ' ' || *end == '\t')
            end++;
    }
    while (end > expr && (end[-1] == ' ' || end[-1] == '\t'))
        end--;
    *end = 0;
    if (!*expr)
        return script_error("let wants name = expr");
    int64_t value;
    if (!script_expr(expr, &value))
        return false;
    script_name_t *found;
    if (script_names_count(&script_syms, name, len, &found))
        return script_error("cannot assign symbol '%.*s'", (int)len, name);
    if (script_names_count(&script_vars, name, len, &found))
        found->value = value;
    else if (!script_names_add(&script_vars, name, len, value))
        return script_error("out of memory");
    return true;
}

/* Nothing answers a read of $FF00-$FFCF or takes a write there. */
static bool script_unassigned(script_space_t space, long addr)
{
    return (space == SCRIPT_CPU || space == SCRIPT_BUS) &&
           addr > SRAM_MMAP_HI && addr < VIA_MMAP_LO;
}

/* bus: and xbus: are stores, so peek, dump and wait refuse them. */
static bool script_readable(script_space_t space)
{
    if (space == SCRIPT_BUS || space == SCRIPT_XBUS)
        return script_error("%s is only for poke", space == SCRIPT_BUS ? "bus:" : "xbus:");
    return true;
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
    else if (space == SCRIPT_XBUS)
        ria_xram_write(addr, value);
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

static void script_put_le(uint32_t v, int bytes)
{
    for (int i = 0; i < bytes; i++)
        fputc((int)(v >> (8 * i)) & 0xFF, script_wav);
}

/* One frame of sound, pulled after every frame the script runs the way a
 * sink pulls it, so the engines keep time with the machine whether or not a
 * wav is being written. With --mute it is silence and costs nothing. */
void script_listen(void)
{
    float out[SCRIPT_AUD_FRAMES * 2];
    aud_render(out, SCRIPT_AUD_FRAMES);
    if (!script_wav)
        return;
    /* aud_render divides a 16-bit sample by 32768, so this gives it back
     * exactly. */
    for (int i = 0; i < SCRIPT_AUD_FRAMES * 2; i++)
        script_put_le((uint16_t)(int16_t)(out[i] * 32768.0f), 2);
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
        if (!script_end(p))
            return false;
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
        if (!script_end(p))
            return false;
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
        if (!script_end(p))
            return false;
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
        if (!script_end(p))
            return false;
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
        if (!script_end(p))
            return false;
        mouse_host_wheel((int)wheel, (int)pan);
        return true;
    }
    if (verb && !strcasecmp(verb, "buttons"))
    {
        long mask;
        if (!script_number(&p, &mask) || mask < 0 || mask > 255)
            return script_error("mouse buttons wants a bitmap 0..255");
        if (!script_end(p))
            return false;
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
        if (!script_end(p))
            return false;
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
            char *comma = script_outside(word, ",");
            if (!*comma)
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
        if (!script_end(p))
            return false;
        tablet_host_wheel((int)wheel, (int)pan);
        return true;
    }
    if (verb && !strcasecmp(verb, "clear"))
    {
        if (!script_end(p))
            return false;
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
        return script_end(p);
    }

    if (!strcasecmp(cmd, "wait"))
    {
        /* A form that does not match leaves the line for the next one, so a
         * name spelled exit needs parentheses to be an address. */
        if (script_keyword(&p, "exit"))
        {
            long frames = SCRIPT_WAIT_FRAMES;
            if (script_more(&p) && (!script_number(&p, &frames) || frames < 0))
                return script_error("wait exit wants a frame budget");
            script_wait = SCRIPT_EXIT;
            script_budget = (unsigned long)frames;
            return script_end(p);
        }
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
                                script_cap_names[cap - script_cap]);
        else
        {
            long want;
            if (!script_address(&p, &script_addr_space, &script_addr) ||
                !script_number(&p, &want) || want < 0 || want > 0xFF)
                return script_error("wait wants a quoted string, or an address "
                                 "and the byte to wait for");
            if (!script_readable(script_addr_space))
                return false;
            if (script_unassigned(script_addr_space, script_addr))
                return script_error("$%04lX is unassigned", script_addr);
            script_addr_want = (uint8_t)want;
            script_wait = SCRIPT_BYTE;
        }
        long frames = SCRIPT_WAIT_FRAMES;
        if (script_more(&p) && (!script_number(&p, &frames) || frames < 0))
            return script_error("wait wants a frame budget");
        script_budget = (unsigned long)frames;
        return script_end(p);
    }

    if (!strcasecmp(cmd, "expect") || !strcasecmp(cmd, "expect-not"))
    {
        bool want = !strcasecmp(cmd, "expect");
        if (script_keyword(&p, "exit"))
        {
            long code;
            if (!script_number(&p, &code))
                return script_error("%s exit wants an exit code", cmd);
            if (!script_end(p))
                return false;
            if (!script_exited())
                return script_error("the program has not exited");
            int got = proc_get_exit_code();
            if (want && got != code)
                return script_error("exit code %d, expected %ld", got, code);
            if (!want && got == code)
                return script_error("did not expect exit code %d", got);
            return true;
        }
        script_cap_t *cap = script_stream(&p);
        char text[256], shown[4 * sizeof text];
        size_t len;
        if (!script_text(&p, text, sizeof text, &len) || !len)
            return script_error("%s wants a quoted string", cmd);
        if (!script_end(p))
            return false;
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

    /* Captured output stays until a check takes it, so a script that starts
     * over discards what the last program wrote. */
    if (!strcasecmp(cmd, "discard"))
    {
        int from = 0, to = 3;
        char *name = script_word(&p);
        if (name)
        {
            for (from = 0; from < 3 && strcasecmp(name, script_cap_names[from]); from++)
                ;
            if (from == 3)
                return script_error("discard wants stdout, stderr or console");
            to = from + 1;
        }
        if (!script_end(p))
            return false;
        for (int i = from; i < to; i++)
            script_cap[i].len = 0;
        return true;
    }

    /* The seed of the program's RNG, which takes effect at its next draw. An
     * int64_t, because a long is 32 bits on Windows. */
    if (!strcasecmp(cmd, "seed"))
    {
        char *word = script_word(&p);
        int64_t seed;
        if (!word)
            return script_error("seed wants a number from 0 to 4294967295");
        if (!script_expr(word, &seed))
            return false;
        if (seed < 0 || seed > UINT32_MAX)
            return script_error("seed wants a number from 0 to 4294967295");
        if (!script_end(p))
            return false;
        script_rng_seed = (uint32_t)seed;
        script_rng_set = true;
        sys_random_set(script_rng_seed);
        return true;
    }

    if (!strcasecmp(cmd, "eof"))
    {
        long frames = SCRIPT_WAIT_FRAMES;
        if (script_more(&p) && (!script_number(&p, &frames) || frames < 0))
            return script_error("eof wants a frame budget");
        script_wait = SCRIPT_EOF;
        script_budget = (unsigned long)frames;
        return script_end(p);
    }

    if (!strcasecmp(cmd, "type"))
    {
        char text[SCRIPT_LINE_MAX];
        if (!script_string(&p, text, sizeof text))
            return script_error("type wants a quoted string");
        long frames = SCRIPT_WAIT_FRAMES;
        if (script_more(&p) && (!script_number(&p, &frames) || frames < 0))
            return script_error("type wants a frame budget");
        if (!script_end(p))
            return false;
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
        if (!script_end(p))
            return false;
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
            int byte = keyboard_ctrl_promote(ch, HID_KEY_NONE);
            if (byte < 0)
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
                /* A word that is neither a key nor a known name is an
                 * unknown key, not an unknown name. */
                int64_t value;
                if (!*script_name_end(name) &&
                    script_lookup(name, strlen(name), &value) == EXPR_UNKNOWN)
                    return script_error("unknown key '%s'", name);
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
        uint8_t led;
        if (!strcasecmp(which, "num"))
            led = KEYBOARD_LED_NUMLOCK;
        else if (!strcasecmp(which, "caps"))
            led = KEYBOARD_LED_CAPSLOCK;
        else if (!strcasecmp(which, "scroll"))
            led = KEYBOARD_LED_SCROLLLOCK;
        else
            return script_error("lock wants num, caps or scroll");
        if (!script_end(p))
            return false;
        keyboard_toggle_lock(led);
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
        if (!script_readable(space))
            return false;
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
        if (!script_end(p))
            return false;
        script_replies = on;
        return true;
    }

    if (!strcasecmp(cmd, "let"))
    {
        while (*p == ' ' || *p == '\t')
            p++;
        char *eq = script_name_end(p);
        size_t len = (size_t)(eq - p);
        while (*eq == ' ' || *eq == '\t')
            eq++;
        if (!len || eq[0] != '=' || eq[1] == '=')
            return script_error("let wants name = expr");
        return script_assign(p, len, eq + 1);
    }

    if (!strcasecmp(cmd, "dump"))
    {
        script_space_t space;
        long addr, count = 1;
        if (!script_address(&p, &space, &addr))
            return script_error("dump wants an address");
        if (!script_readable(space))
            return false;
        if (script_more(&p) && (!script_number(&p, &count) || count < 1))
            return script_error("dump wants a byte count");
        if (!script_end(p))
            return false;
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

    if (!strcasecmp(cmd, "status"))
    {
        if (!script_end(p))
            return false;
        char status[SCRIPT_STATUS_SIZE];
        script_status(status, sizeof status);
        printf(script_replies ? "ok %s\n" : "%s\n", status);
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
        if (!script_end(p))
            return false;
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

    if (!strcasecmp(cmd, "wav"))
    {
        char path[SCRIPT_LINE_MAX];
        if (!script_string(&p, path, sizeof path) || !path[0])
            return script_error("wav wants a quoted path");
        long frames = 1;
        if (script_more(&p) && (!script_number(&p, &frames) || frames < 0 ||
                                frames > SCRIPT_WAV_MAX_FRAMES))
            return script_error("wav wants a frame count");
        if (!script_end(p))
            return false;
        script_wav = os_fopen(path, "wb");
        if (!script_wav)
            return script_error("cannot write '%s'", path);
        uint32_t data = (uint32_t)frames * SCRIPT_AUD_FRAMES * 4;
        fwrite("RIFF", 1, 4, script_wav);
        script_put_le(36 + data, 4);
        fwrite("WAVEfmt ", 1, 8, script_wav);
        script_put_le(16, 4);
        script_put_le(1, 2); /* PCM */
        script_put_le(2, 2); /* stereo */
        script_put_le(SCRIPT_AUD_RATE, 4);
        script_put_le(SCRIPT_AUD_RATE * 4, 4);
        script_put_le(4, 2);
        script_put_le(16, 2);
        fwrite("data", 1, 4, script_wav);
        script_put_le(data, 4);
        script_wait = SCRIPT_WAV;
        script_budget = (unsigned long)frames;
        return true;
    }

    if (!strcasecmp(cmd, "shot"))
    {
        char path[SCRIPT_LINE_MAX]; /* a token cannot outrun its own line */
        if (!script_string(&p, path, sizeof path))
            return script_error("shot wants a quoted path");
        if (!script_end(p))
            return false;
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
        if (!script_end(p))
            return false;
        const char *why = "";
        if (saving ? !state_save_file(path, &why) : !state_load_file(path, &why))
            return script_error("cannot %s '%s': %s", saving ? "save" : "load",
                                path, why);
        return true;
    }

    /* The script is UTF-8. An installed ROM's path stays a host path in
     * UTF-8, while a name or a path that the machine resolves is converted to
     * OEM, which only ever shortens it. */
    if (!strcasecmp(cmd, "install"))
    {
        char path[SCRIPT_LINE_MAX];
        if (!script_string(&p, path, sizeof path) || !path[0])
            return script_error("install wants a quoted path");
        char *as = script_more(&p) ? script_word(&p) : NULL;
        if (!script_end(p))
            return false;
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
        if (!script_end(p))
            return false;
        oem_from_utf8(name, name, strlen(name) + 1);
        if (!rom_alias_remove(name))
            return script_error("nothing installed as '%s'", name);
        return true;
    }

    if (!strcasecmp(cmd, "load"))
    {
        char path[SCRIPT_LINE_MAX], oem[SCRIPT_LINE_MAX];
        if (!script_string(&p, path, sizeof path) || !path[0])
            return script_error("load wants a quoted path");
        if (!script_end(p))
            return false;
        oem_from_utf8(path, oem, sizeof oem);
        /* A running program would have its memory written out from under it,
         * so a load happens only between programs. */
        if (sys_active())
            return script_error("load wants a stopped machine");
        vtkeys_paste_cancel();
        if (!proc_boot(oem, 0, NULL, PROC_UNCHAIN))
        {
            const char *why = proc_boot_error();
            return script_error("%.*s", (int)strcspn(why, "\n"), why);
        }
        script_halted = false;
        sys_commit();
        /* The symbols now come from the new program's .sym. An installed
         * :NAME has its .sym beside the file that was installed. */
        char host[SCRIPT_LINE_MAX];
        const char *rom = rom_alias_resolve(oem);
        if (!rom)
        {
            /* The machine takes either slash, so the .sym lookup does too. */
            snprintf(host, sizeof host, "%s", strncasecmp(path, "FS:", 3) ? path : path + 3);
#ifndef _WIN32
            for (char *s = host; *s; s++)
                if (*s == '\\')
                    *s = '/';
#endif
            rom = host;
        }
        char why[SCRIPT_LINE_MAX];
        if (!script_symbols(rom, why, sizeof why))
            return script_error("%s", why);
        return true;
    }

    if (!strcasecmp(cmd, "sys"))
    {
        char *what = script_word(&p);
        if (!what || (strcasecmp(what, "run") && strcasecmp(what, "stop") &&
                      strcasecmp(what, "break")))
            return script_error("sys wants run, stop or break");
        if (!script_end(p))
            return false;
        /* A stop of a machine that already stopped keeps its exit. */
        if (!strcasecmp(what, "run"))
            script_halted = false;
        else if (sys_running())
            script_halted = true;
        if (!strcasecmp(what, "run"))
            sys_run();
        else if (!strcasecmp(what, "stop"))
            sys_stop();
        else
            sys_break_request();
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
        if (script_exited())
            break;
        if (script_spend())
            return false;
        return script_error("timed out waiting for the program to exit");
    case SCRIPT_EOF:
        /* The moment console.c ends a piped stdin: a read is waiting and
         * everything typed has been taken, so no typed byte is lost. */
        if (std_stdin_waiting() && com_input_idle())
        {
            std_stdin_eof();
            break;
        }
        if (script_spend())
            return false;
        return script_error("timed out ending stdin; the program is not reading it");
    case SCRIPT_WAV:
    {
        if (script_spend())
            return false;
        FILE *f = script_wav;
        script_wav = NULL;
        if (ferror(f) | fclose(f))
            return script_error("cannot write the wav");
        break;
    }
    }
    script_wait = SCRIPT_IDLE;
    return true;
}

/* A program's xreg blanks the gamepad block until the next report, so the other
 * hosts restate their pads on every poll, and a script restates its own before
 * every frame. */
static void script_gamepads_restate(void)
{
    for (int i = 0; i < 4; i++)
        if (script_gamepad[i].connected)
            script_gamepad_publish(i);
}

void script_task(void)
{
    char line[SCRIPT_LINE_MAX];
    script_gamepads_restate();
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
            "a failed check names the line and the status, then exits 1):\n"
            "  run [frames]              let frames elapse (default 1)\n"
            "  wait [stdout|stderr|console] \"text\" [frames]\n"
            "                                      run until the program writes it\n"
            "                                      (default stdout)\n"
            "  wait [xram:]<addr> <byte> [frames]  run until that byte reads that value\n"
            "  wait exit [frames]        run until the program exits\n"
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
            "  expect [stdout|stderr|console] \"text\"\n"
            "  expect-not [stdout|stderr|console] \"text\"\n"
            "                                      check the stream since its last\n"
            "                                      match (default stdout)\n"
            "  expect exit <code>        check the exit code of the program\n"
            "  expect-not exit <code>\n"
            "  discard [stdout|stderr|console]     drop what no check has taken\n"
            "                                      (default all three)\n"
            "  eof [frames]              end stdin once the program has read what was typed\n"
            "  peek [xram:]<addr> <byte>...        compare memory\n"
            "  poke [xram:|bus:|xbus:]<addr> <byte>...\n"
            "                                      write memory; bus: and xbus: store\n"
            "                                      the way the 6502 and the RIA do\n"
            "  dump [xram:]<addr> [count]          print memory as hex\n"
            "  status                              PC A X Y SP P, OP BUSY RESB, EXIT\n"
            "                                      once the program exits, SEED,\n"
            "                                      MEMSEED or MEMFILL, then ROM\n"
            "  seed <n>                  set the seed of the program's RNG, 0 to\n"
            "                            4294967295\n"
            "  crc                                 the canvas as a CRC-32\n"
            "  mark, expect-same, expect-changed   the canvas against a remembered one\n"
            "  shot \"file.png\"           write the canvas\n"
            "  wav \"file.wav\" [frames]   record that many frames of sound (default 1)\n"
            "  state save \"file\" / state load \"file\"   the machine, written down\n"
            "  install \"path\" [NAME]     put a file on the null drive as :NAME\n"
            "  remove <NAME>             take it back off\n"
            "  load \"path\"               boot a program on a stopped machine\n"
            "  sys run|stop|break        start, stop, or interrupt the machine\n"
            "  reply [on|off]            answer every command on stdout, for a\n"
            "                            driver on the other end of a pipe\n"
            "  let name = expr           set a variable that later lines can use\n"
            "\nEvery number is a Python 3 expression, such as 255, $FF or XRAM_PSG+5.\n"
            "Spaces separate arguments, so an expression with spaces must be in\n"
            "parentheses: poke xram:(XRAM_PSG + 5) $80. Names come from variables\n"
            "and from the .sym beside the ROM. Floats are not allowed, so use // to\n"
            "divide. A command that waits fails when its [frames] run out, 600 by\n"
            "default.\n");
}

bool script_load(const char *path, const char *rom)
{
    char why[SCRIPT_LINE_MAX];
    script_names_clear(&script_vars);
    if (!script_symbols(rom, why, sizeof why))
    {
        fprintf(stderr, "rp6502-emu: %s\n", why);
        return false;
    }
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
    com_set_tx_tap(script_console_tap);
    script_line_no = 0;
    script_wait = SCRIPT_IDLE;
    script_budget = 0;
    for (int i = 0; i < 3; i++)
        script_cap[i].len = 0;
    script_stderr_midline = false;
    aud_set_sink_rate(SCRIPT_AUD_RATE);
    script_marked = false;
    script_rng_set = false;
    script_halted = false;
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

void script_set_memory(bool memfill_set, uint8_t memfill, uint32_t memseed)
{
    script_memfill_set = memfill_set;
    script_memfill = memfill;
    script_memseed = memseed;
}
