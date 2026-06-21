/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "mon/dsk.h"
#include "mon/fil.h"
#include "mon/hlp.h"
#include "mon/mon.h"
#include "mon/ram.h"
#include "mon/rom.h"
#include "mon/set.h"
#include "mon/uf2.h"
#include "net/cyw.h"
#include "str/rln.h"
#include "str/str.h"
#include "sys/com.h"
#include "sys/mem.h"
#include "sys/ria.h"
#include "sys/sys.h"
#include "usb/usb.h"
#include <fatfs/ff.h>
#include <littlefs/lfs.h>
#include <pico/stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#if defined(DEBUG_RIA_MON) || defined(DEBUG_RIA_MON_MON)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define MON_RESPONSE_BUF_SIZE 128
// 16 = longest response chain (set with no args queues 15) + 1 free-slot margin.
#define MON_RESPONSE_FN_COUNT 16
// Minimum column budget remaining after the indent for wrap-with-indent
// to engage. If the BEL marker lands too close to the right edge the
// indent is suppressed and wrapped lines fall back to column 0.
#define MON_RESPONSE_INDENT_MIN_WRAP 20
// Double-buffer the response stream: one is being drained while the other
// stages the producer's next fill, so a word that crosses the fill
// boundary can be looked ahead without copying or shrinking the buffer
// the producer sees.
static char mon_response_buf_a[MON_RESPONSE_BUF_SIZE];
static char mon_response_buf_b[MON_RESPONSE_BUF_SIZE];
static char *mon_response_cur = mon_response_buf_a;
static char *mon_response_next = mon_response_buf_b;
static bool mon_response_next_loaded;
static mon_response_fn mon_response_fn_list[MON_RESPONSE_FN_COUNT];
static const char *mon_response_str[MON_RESPONSE_FN_COUNT];
static int mon_response_state[MON_RESPONSE_FN_COUNT] =
    {[0 ... MON_RESPONSE_FN_COUNT - 1] = -1};
static int mon_more_rows_left = 23; // default 24-row screen less prompt; reset to term_height-1 before use
static int mon_response_col;
static int mon_response_indent;
static int mon_response_indent_pending;
static bool mon_response_width_aware;
static int mon_response_pos = -1;
static bool mon_needs_prompt = true;
static bool mon_needs_read_line = true;
static bool mon_needs_break = false;
static mon_confirm_fn mon_confirm_cb; // pending YES/no confirmation action
static enum {
    MON_MORE_OFF,
    MON_MORE_START,
    MON_MORE_END,
    MON_MORE_WAIT,
    MON_MORE_WAIT_ESC,
    MON_MORE_WAIT_CSI,
} mon_more_state;

typedef void (*mon_command_fn)(const char *);
__in_flash("mon_commands") static struct
{
    const char *const cmd;
    mon_command_fn func;
} const MON_COMMANDS[] = {
    {STR_HELP, hlp_mon_help},
    {STR_H, hlp_mon_help},
    {STR_QUESTION_MARK, hlp_mon_help},
    {STR_STATUS, sys_mon_status},
    {STR_SET, set_mon_set},
    {STR_LS, fil_mon_dir},
    {STR_DIR, fil_mon_dir},
    {STR_CD, fil_mon_chdir},
    {STR_CHDIR, fil_mon_chdir},
    {STR_MKDIR, fil_mon_mkdir},
    {STR_LOAD, rom_mon_load},
    {STR_INFO, rom_mon_info},
    {STR_INSTALL, rom_mon_install},
    {STR_REMOVE, rom_mon_remove},
    {STR_REBOOT, sys_mon_reboot},
    {STR_RESET, sys_mon_reset},
    {STR_FLASH, uf2_mon_flash},
    {STR_UPLOAD, fil_mon_upload},
    {STR_UNLINK, fil_mon_unlink},
    {STR_RM, fil_mon_unlink},
    {STR_COPY, fil_mon_copy},
    {STR_CP, fil_mon_copy},
    {STR_MOVE, fil_mon_move},
    {STR_MV, fil_mon_move},
    {STR_BINARY, ram_mon_binary},
    {STR_DISK, dsk_mon_disk},
};
static const size_t MON_COMMANDS_COUNT = sizeof MON_COMMANDS / sizeof *MON_COMMANDS;

// Returns NULL if not found. Advances buf to start of args.
static mon_command_fn mon_command_lookup(const char **buf)
{
    while (**buf == ' ')
        (*buf)++;
    const char *cmd = *buf;
    if (!*cmd)
        return NULL;
    const char *tok = str_parse_string(buf);
    if (!tok)
        return NULL;
    bool is_addr = true;
    for (const char *p = tok; *p; p++)
    {
        uint8_t ch = *p;
        if (!isxdigit(ch) && ch != '-' && ch != ':')
        {
            is_addr = false;
            break;
        }
    }
    // "cd" is the chdir command, not a hex address.
    if (!strcasecmp(tok, STR_CD))
        is_addr = false;
    // 0:-7: and MSC0:-MSC7:
    if (fil_drive_exists(cmd))
    {
        *buf = cmd;
        return fil_mon_chdrive;
    }
    // address command
    if (is_addr)
    {
        *buf = cmd;
        return ram_mon_address;
    }
    for (size_t i = 0; i < MON_COMMANDS_COUNT; i++)
        if (!strcasecmp(tok, MON_COMMANDS[i].cmd))
            return MON_COMMANDS[i].func;
    return NULL;
}

bool mon_command_exists(const char *buf)
{
    return !!mon_command_lookup(&buf);
}

static void mon_enter(bool timeout, const char *buf)
{
    (void)timeout;
    assert(!timeout);
    if (mon_needs_read_line) // cancelled
        return;
    mon_more_rows_left = rln_get_term_height() - 1;
    mon_needs_prompt = true;
    mon_needs_read_line = true;
    const char *args = buf;
    mon_command_fn func = mon_command_lookup(&args);
    if (func)
    {
        func(args);
        return;
    }
    if (rom_load_installed(buf))
        return;
    // Suppress error for empty lines
    if (!str_parse_end(buf))
        mon_add_response_utf8(S(STR_ERR_UNKNOWN_COMMAND));
}

static void mon_confirm_enter(bool timeout, const char *buf)
{
    (void)timeout;
    assert(!timeout);
    if (mon_needs_read_line) // cancelled (Ctrl-C poke / break)
    {
        mon_confirm_cb = NULL;
        return;
    }
    mon_more_rows_left = rln_get_term_height() - 1;
    mon_needs_prompt = true;
    mon_needs_read_line = true;
    mon_confirm_fn cb = mon_confirm_cb;
    mon_confirm_cb = NULL;
    // The typed token is OEM (active code page); the confirm word is UTF-8, so
    // convert it to OEM, then compare with the code-page-aware str_oem_eq.
    char yes[16];
    snprintf_utf8(yes, sizeof(yes), "%s", S(STR_MON_CONFIRM_YES));
    const char *tok = str_parse_string(&buf);
    if (cb && tok && str_oem_eq(tok, yes) && str_parse_end(buf))
        cb();
}

void mon_response_confirm(mon_confirm_fn cb)
{
    mon_confirm_cb = cb;
}

static int mon_utf8_response(char *buf, size_t buf_size, int state)
{
    if (state < 0)
        return state;
    const char *str = mon_response_str[0];
    const char *p = str + state;
    size_t i = 0;
    while (i + 1 < buf_size && *p)
        buf[i++] = (char)str_utf8_to_oem(&p);
    buf[i] = 0;
    if (!*p)
        return -1;
    return (int)(p - str);
}

static const char *mon_lfs_lookup(int result)
{
    switch (-result)
    {
    case LFS_ERR_IO: // -5
        return S(STR_ERR_LFS_IO);
    case LFS_ERR_CORRUPT: // -84
        return S(STR_ERR_LFS_CORRUPT);
    case LFS_ERR_NOENT: // -2
        return S(STR_ERR_LFS_NOENT);
    case LFS_ERR_EXIST: // -17
        return S(STR_ERR_LFS_EXIST);
    case LFS_ERR_NOTDIR: // -20
        return S(STR_ERR_LFS_NOTDIR);
    case LFS_ERR_ISDIR: // -21
        return S(STR_ERR_LFS_ISDIR);
    case LFS_ERR_NOTEMPTY: // -39
        return S(STR_ERR_LFS_NOTEMPTY);
    case LFS_ERR_BADF: // -9
        return S(STR_ERR_LFS_BADF);
    case LFS_ERR_FBIG: // -27
        return S(STR_ERR_LFS_FBIG);
    case LFS_ERR_INVAL: // -22
        return S(STR_ERR_LFS_INVAL);
    case LFS_ERR_NOSPC: // -28
        return S(STR_ERR_LFS_NOSPC);
    case LFS_ERR_NOMEM: // -12
        return S(STR_ERR_LFS_NOMEM);
    case LFS_ERR_NOATTR: // -61
        return S(STR_ERR_LFS_NOATTR);
    case LFS_ERR_NAMETOOLONG: // -36
        return S(STR_ERR_LFS_NAMETOOLONG);
    default:
        return NULL;
    }
}

static const char *mon_fatfs_lookup(int fresult)
{
    switch (fresult)
    {
    case FR_DISK_ERR: // 1
        return S(STR_ERR_FATFS_DISK_ERR);
    case FR_INT_ERR: // 2
        return S(STR_ERR_FATFS_INT_ERR);
    case FR_NOT_READY: // 3
        return S(STR_ERR_FATFS_NOT_READY);
    case FR_NO_FILE: // 4
        return S(STR_ERR_FATFS_NO_FILE);
    case FR_NO_PATH: // 5
        return S(STR_ERR_FATFS_NO_PATH);
    case FR_INVALID_NAME: // 6
        return S(STR_ERR_FATFS_INVALID_NAME);
    case FR_DENIED: // 7
        return S(STR_ERR_FATFS_DENIED);
    case FR_EXIST: // 8
        return S(STR_ERR_FATFS_EXIST);
    case FR_INVALID_OBJECT: // 9
        return S(STR_ERR_FATFS_INVALID_OBJECT);
    case FR_WRITE_PROTECTED: // 10
        return S(STR_ERR_FATFS_WRITE_PROTECTED);
    case FR_INVALID_DRIVE: // 11
        return S(STR_ERR_FATFS_INVALID_DRIVE);
    case FR_NOT_ENABLED: // 12
        return S(STR_ERR_FATFS_NOT_ENABLED);
    case FR_NO_FILESYSTEM: // 13
        return S(STR_ERR_FATFS_NO_FILESYSTEM);
    case FR_MKFS_ABORTED: // 14
        return S(STR_ERR_FATFS_MKFS_ABORTED);
    case FR_TIMEOUT: // 15
        return S(STR_ERR_FATFS_TIMEOUT);
    case FR_LOCKED: // 16
        return S(STR_ERR_FATFS_LOCKED);
    case FR_NOT_ENOUGH_CORE: // 17
        return S(STR_ERR_FATFS_NOT_ENOUGH_CORE);
    case FR_TOO_MANY_OPEN_FILES: // 18
        return S(STR_ERR_FATFS_TOO_MANY_OPEN_FILES);
    case FR_INVALID_PARAMETER: // 19
        return S(STR_ERR_FATFS_INVALID_PARAMETER);
    default:
        return NULL;
    }
}

static int mon_err_response(char *buf, size_t buf_size, int state,
                            const char *(*lookup)(int))
{
    if (state < 0)
        return state;
    const char *err_str = lookup(state);
    if (err_str != NULL)
        snprintf_utf8(buf, buf_size, "%s", err_str);
    else
        snprintf_utf8(buf, buf_size, S(STR_ERR_UNKNOWN_NUMBER), state);
    return -1;
}

static int mon_lfs_response(char *buf, size_t buf_size, int state)
{
    return mon_err_response(buf, buf_size, state, mon_lfs_lookup);
}

static int mon_fatfs_response(char *buf, size_t buf_size, int state)
{
    return mon_err_response(buf, buf_size, state, mon_fatfs_lookup);
}

static void mon_append_response(mon_response_fn fn, const char *str, int state)
{
    assert(state >= 0);
    int i = 0;
    for (; i < MON_RESPONSE_FN_COUNT; i++)
    {
        if (!mon_response_fn_list[i])
        {
            // Suppress consecutive duplicates — no value in showing the same
            // string twice in a row.
            if (i > 0 && fn == mon_response_fn_list[i - 1] && str == mon_response_str[i - 1])
                return;
            mon_response_fn_list[i] = fn;
            mon_response_str[i] = str;
            mon_response_state[i] = state;
            return;
        }
    }
    i--;
    if (mon_response_str[i] == S(STR_ERR_MONITOR_RESPONSE_OVERFLOW))
        return;
    mon_response_fn_list[i] = mon_utf8_response;
    mon_response_str[i] = S(STR_ERR_MONITOR_RESPONSE_OVERFLOW);
    mon_response_state[i] = 0;
}

// Reset a response slot to empty (state -1 marks a free slot).
static void mon_clear_slot(int i)
{
    mon_response_fn_list[i] = NULL;
    mon_response_str[i] = NULL;
    mon_response_state[i] = -1;
}

static void mon_next_response(void)
{
    int i = 0;
    for (; i < MON_RESPONSE_FN_COUNT - 1; i++)
    {
        mon_response_fn_list[i] = mon_response_fn_list[i + 1];
        mon_response_str[i] = mon_response_str[i + 1];
        mon_response_state[i] = mon_response_state[i + 1];
    }
    mon_clear_slot(i);
}

static void mon_break_response(void)
{
    mon_needs_break = false;
    mon_response_pos = -1;
    mon_response_col = 0;
    mon_response_indent = 0;
    mon_response_indent_pending = 0;
    mon_response_width_aware = false;
    mon_response_next_loaded = false;
    mon_more_rows_left = rln_get_term_height() - 1;
    for (int i = 0; i < MON_RESPONSE_FN_COUNT && mon_response_state[i] >= 0; i++)
    {
        mon_response_fn_list[i](mon_response_cur, MON_RESPONSE_BUF_SIZE, -1);
        mon_clear_slot(i);
    }
}

void mon_add_response_fn(mon_response_fn fn)
{
    mon_append_response(fn, NULL, 0);
}

void mon_add_response_fn_state(mon_response_fn fn, int state)
{
    mon_append_response(fn, NULL, state);
}

void mon_add_response_utf8(const char *utf8)
{
    mon_append_response(mon_utf8_response, utf8, 0);
}

void mon_add_response_lfs(int result)
{
    if (result < 0)
        mon_append_response(mon_lfs_response, NULL, -result);
}

void mon_add_response_fatfs(int fresult)
{
    if (fresult > 0)
        mon_append_response(mon_fatfs_response, NULL, fresult);
}

static void mon_more(void)
{
    if (mon_needs_break)
    {
        // Don't erase a prompt we haven't drawn yet; just go to OFF.
        if (mon_more_state == MON_MORE_START)
        {
            mon_more_state = MON_MORE_OFF;
            return;
        }
        mon_more_state = MON_MORE_END;
    }
    switch (mon_more_state)
    {
    case MON_MORE_START:
        printf_utf8(S(STR_MON_MORE_SHOW));
        mon_more_state = MON_MORE_WAIT;
        break;
    case MON_MORE_END:
        printf_utf8(S(STR_MON_MORE_ERASE));
        mon_more_rows_left = rln_get_term_height() - 1;
        mon_more_state = MON_MORE_OFF;
        break;
    default: // MON_MORE_WAIT, MON_MORE_WAIT_ESC, MON_MORE_WAIT_CSI
    {
        // Non-blocking byte-driven drain: any keypress advances past
        // --more--, but ESC-prefixed sequences (arrow keys, F-keys,
        // Alt+key, anything kbd.c emits via vt100/vt220) are consumed
        // whole so their tail doesn't leak into the next prompt.
        int ch;
        while ((ch = stdio_getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT)
        {
            if (mon_more_state == MON_MORE_WAIT)
            {
                if (ch == 3 || ch == 'q' || ch == 'Q')
                    mon_needs_break = true;
                if (ch == '\n')
                    ; /* no-op */
                else if (ch == '\33')
                    mon_more_state = MON_MORE_WAIT_ESC;
                else
                    mon_more_state = MON_MORE_END;
            }
            else if (mon_more_state == MON_MORE_WAIT_ESC)
            {
                if (ch == '[' || ch == 'O')
                    mon_more_state = MON_MORE_WAIT_CSI;
                else
                    mon_more_state = MON_MORE_END;
            }
            else // MON_MORE_WAIT_CSI
            {
                if (ch >= 0x40 && ch <= 0x7E)
                    mon_more_state = MON_MORE_END;
            }
            if (mon_more_state == MON_MORE_END)
                return;
        }
        break;
    }
    }
}

void mon_task(void)
{
    // The monitor must never print while 6502 is running.
    if (main_active())
        return;
    if (mon_more_state)
    {
        mon_more();
        return;
    }
    if (mon_needs_break)
    {
        mon_break_response();
        return;
    }
    // If cur is exhausted and next is loaded, swap pointers — no copy,
    // so streaming can resume in the same tick.
    if (mon_response_pos == -1 && mon_response_next_loaded)
    {
        char *tmp = mon_response_cur;
        mon_response_cur = mon_response_next;
        mon_response_next = tmp;
        mon_response_pos = 0;
        mon_response_next_loaded = false;
    }
    // Prime the staged buffer whenever empty so the streaming lookahead
    // can span the fill boundary. One producer call per tick.
    if (!mon_response_next_loaded && mon_response_state[0] >= 0)
    {
        mon_response_next[0] = 0;
        mon_response_state[0] = (mon_response_fn_list[0])(
            mon_response_next, MON_RESPONSE_BUF_SIZE, mon_response_state[0]);
        mon_response_next_loaded = (mon_response_next[0] != 0);
        if (mon_response_state[0] < 0)
            mon_next_response();
        return;
    }
    // Flush the current response buffer
    if (mon_response_pos >= 0)
    {
        int width = rln_get_term_width();
        char c;
        while ((c = mon_response_cur[mon_response_pos]) && com_putchar_ready())
        {
            // BEL marks the indent column for subsequent wraps; consume
            // it silently and don't advance the column. Must run before
            // the --more-- check so BEL never pauses. The indent_pending
            // guard preserves indent emission ordering.
            if (mon_response_indent_pending == 0 && c == '\a')
            {
                mon_response_indent =
                    (width - mon_response_col >= MON_RESPONSE_INDENT_MIN_WRAP)
                        ? mon_response_col
                        : 0;
                mon_response_pos++;
                continue;
            }
            // Any remaining path emits a printable byte or a newline. If
            // we have no row left for it, pause first; we resume here
            // when --more-- is dismissed.
            if (mon_more_rows_left <= 0)
            {
                mon_more_state = MON_MORE_START;
                break;
            }
            // Emit one queued indent space per iteration after a
            // paginator-injected wrap, so wrapped continuation lines
            // resume at the column marked by the producer's BEL.
            if (mon_response_indent_pending > 0)
            {
                putchar(' ');
                mon_response_indent_pending--;
                mon_response_col++;
                continue;
            }
            // Word wrap on space: peek the next word's length. The
            // lookahead spans into the staged buffer when the word crosses
            // the fill boundary, so the wrap decision is made on the full
            // word without copying anything.
            if (!mon_response_width_aware && c == ' ')
            {
                int n = mon_response_pos + 1;
                while (mon_response_cur[n] && mon_response_cur[n] != ' ' &&
                       mon_response_cur[n] != '\n' && mon_response_cur[n] != '\r')
                    n++;
                int next_word_len = n - mon_response_pos - 1;
                bool word_complete = mon_response_cur[n] != 0;
                if (!word_complete && mon_response_next_loaded)
                {
                    int m = 0;
                    while (mon_response_next[m] && mon_response_next[m] != ' ' &&
                           mon_response_next[m] != '\n' && mon_response_next[m] != '\r')
                        m++;
                    next_word_len += m;
                    word_complete = mon_response_next[m] != 0 ||
                                    mon_response_state[0] < 0;
                }
                else if (!word_complete)
                {
                    word_complete = (mon_response_state[0] < 0);
                }
                if (!word_complete ||
                    mon_response_col + 1 + next_word_len > width)
                {
                    putchar('\n');
                    mon_response_pos++; // drop the space
                    mon_response_col = 0;
                    mon_response_indent_pending = mon_response_indent;
                    mon_more_rows_left--;
                    continue;
                }
            }
            // Hard newline injection for a glyph that would overflow the line —
            // catches words longer than the line that the word-wrap branch above
            // could not break. Bytes 0x20-0xFF are one SBCS OEM glyph each, so
            // each counts one column. Suppressed once a producer emits a control
            // byte (see the ladder below) whose width we can't track.
            if (!mon_response_width_aware && (unsigned char)c >= 0x20 && mon_response_col >= width)
            {
                putchar('\n');
                mon_response_col = 0;
                mon_response_indent_pending = mon_response_indent;
                mon_more_rows_left--;
                continue; // re-loop without advancing pos
            }
            putchar(c);
            mon_response_pos++;
            if (c == '\n')
            {
                mon_response_col = 0;
                mon_response_indent = 0;
                mon_more_rows_left--;
            }
            else if (c == '\r')
            {
                mon_response_col = 0;
            }
            else if (c == '\b')
            {
                if (mon_response_col > 0)
                    mon_response_col--;
            }
            else if ((unsigned char)c < 0x20)
            {
                // A control byte (ESC sequence, tab, ...) whose on-screen width we
                // can't track; stop wrap/column injection for the rest of the chain.
                mon_response_width_aware = true;
            }
            else
            {
                mon_response_col++;
            }
        }
        if (!c)
            mon_response_pos = -1;
        return;
    }
    // Wait for any active subsystem before issuing the next prompt.
    if (ram_active() ||
        rom_active() ||
        fil_active() ||
        uf2_active() ||
        dsk_active() ||
        usb_boot_enumerating())
        return;
    // The monitor has control
    if (mon_needs_prompt)
    {
        if (mon_confirm_cb)
            printf_utf8(S(STR_MON_CONFIRM_PROMPT));
        else
            printf("]");
        mon_needs_prompt = false;
        return;
    }
    if (mon_needs_read_line)
    {
        mon_needs_read_line = false;
        mon_response_col = 0;
        mon_response_width_aware = false;
        ria_get_sigint(); // discard any SIGINT raised while monitor was idle
        if (mon_confirm_cb)
            rln_read_line_no_history(mon_confirm_enter); // don't record YES/no
        else
            rln_read_line(mon_enter);
        return;
    }
    if (ria_get_sigint())
    {
        mon_needs_prompt = true;
        mon_needs_read_line = true;
        rln_poke("\x03");
        while (stdio_getchar_timeout_us(0) != PICO_ERROR_TIMEOUT)
            tight_loop_contents();
    }
}

void mon_stop(void)
{
    // Graceful return to a fresh prompt; dismisses --more-- if shown.
    if (mon_more_state)
    {
        mon_needs_break = true;
        mon_more();
    }
    mon_confirm_cb = NULL; // a break/stop cancels any pending confirmation
    mon_needs_prompt = true;
    mon_needs_read_line = true;
}

void mon_break(void)
{
    mon_needs_break = true;
    mon_stop();
}
