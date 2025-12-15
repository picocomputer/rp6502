/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "mon/fil.h"
#include "mon/hlp.h"
#include "mon/mon.h"
#include "mon/ram.h"
#include "mon/rom.h"
#include "mon/set.h"
#include "net/cyw.h"
#include "str/str.h"
#include "sys/com.h"
#include "sys/mem.h"
#include "sys/rln.h"
#include "sys/sys.h"
#include <pico.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#if defined(DEBUG_RIA_MON) || defined(DEBUG_RIA_MON_MON)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define MON_RESPONSE_FN_COUNT 4
static mon_response_fn mon_response_fn_list[MON_RESPONSE_FN_COUNT];
static const char *mon_response_str_list[MON_RESPONSE_FN_COUNT];
static int mon_response_state = -1;
static int mon_response_pos = -1;
static bool mon_needs_newline = true;
static bool mon_needs_prompt = true;
static bool mon_needs_read_line = false;

typedef void (*mon_function)(const char *, size_t);
__in_flash("mon_commands") static struct
{
    const char *const cmd;
    mon_function func;
} const MON_COMMANDS[] = {
    {STR_HELP, hlp_mon_help},
    {STR_H, hlp_mon_help},
    {STR_QUESTION_MARK, hlp_mon_help},
    {STR_STATUS, sys_mon_status},
    {STR_SET, set_mon_set},
    {STR_LS, fil_mon_ls},
    {STR_DIR, fil_mon_ls},
    {STR_CD, fil_mon_chdir},
    {STR_CHDIR, fil_mon_chdir},
    {STR_MKDIR, fil_mon_mkdir},
    {STR_LOAD, rom_mon_load},
    {STR_INFO, rom_mon_info},
    {STR_INSTALL, rom_mon_install},
    {STR_REMOVE, rom_mon_remove},
    {STR_REBOOT, sys_mon_reboot},
    {STR_RESET, sys_mon_reset},
    {STR_UPLOAD, fil_mon_upload},
    {STR_UNLINK, fil_mon_unlink},
    {STR_BINARY, ram_mon_binary},
};
static const size_t MON_COMMANDS_COUNT = sizeof MON_COMMANDS / sizeof *MON_COMMANDS;

// Returns NULL if not found. Advances buf to start of args.
static mon_function mon_command_lookup(const char **buf, size_t buflen)
{
    size_t i;
    for (i = 0; i < buflen; i++)
    {
        if ((*buf)[i] != ' ')
            break;
    }
    const char *cmd = (*buf) + i;
    bool is_maybe_addr = false;
    bool is_not_addr = false;
    for (; i < buflen; i++)
    {
        uint8_t ch = (*buf)[i];
        if (isxdigit(ch))
            is_maybe_addr = true;
        else if (ch == ' ')
            break;
        else
            is_not_addr = true;
    }
    size_t cmd_len = (*buf) + i - cmd;
    for (; i < buflen; i++)
    {
        if ((*buf)[i] != ' ')
            break;
    }
    // cd for chdir, 00cd for r/w address
    if (cmd_len == 2 && !strncasecmp(cmd, "cd", cmd_len))
        is_not_addr = true;
    // address command
    if (is_maybe_addr && !is_not_addr)
    {
        *buf = cmd;
        return ram_mon_address;
    }
    // *0:-*9: is chdrive
    if (cmd_len >= 2 && cmd[cmd_len - 1] == ':' &&
        cmd[cmd_len - 2] >= '0' && cmd[cmd_len - 2] <= '9')
    {
        *buf = cmd;
        return fil_mon_chdrive;
    }
    *buf += i;
    for (i = 0; i < MON_COMMANDS_COUNT; i++)
    {
        if (cmd_len == strlen(MON_COMMANDS[i].cmd))
            if (!strncasecmp(cmd, MON_COMMANDS[i].cmd, cmd_len))
                return MON_COMMANDS[i].func;
    }
    return NULL;
}

bool mon_command_exists(const char *buf, size_t buflen)
{
    return !!mon_command_lookup(&buf, buflen);
}

static void mon_enter(bool timeout, const char *buf, size_t length)
{
    (void)timeout;
    assert(!timeout);
    mon_needs_prompt = true;
    const char *args = buf;
    mon_function func = mon_command_lookup(&args, length);
    if (func)
        return func(args, length - (args - buf));
    if (rom_load_installed(buf, length))
        return;
    // Supress error for empty lines
    for (const char *b = buf; b < args; b++)
        if (b[0] != ' ')
            return mon_add_response_str(STR_ERR_UNKNOWN_COMMAND);
}

static int mon_next_response(void)
{
    int i = 0;
    for (; i < MON_RESPONSE_FN_COUNT - 1; i++)
    {
        mon_response_fn_list[i] = mon_response_fn_list[i + 1];
        mon_response_str_list[i] = mon_response_str_list[i + 1];
    }
    mon_response_fn_list[i] = 0;
    mon_response_str_list[i] = 0;
    if (mon_response_fn_list[0] != NULL)
        return 0;
    else
        return -1;
}

static void mon_append_response(mon_response_fn fn, const char *str)
{
    int i = 0;
    for (; i < MON_RESPONSE_FN_COUNT; i++)
    {
        if (!mon_response_fn_list[i])
        {
            mon_response_fn_list[i] = fn;
            mon_response_str_list[i] = str;
            if (i == 0)
                mon_response_state = 0;
            return;
        }
    }
}

static int mon_str_response(char *buf, size_t buf_size, int state)
{
    size_t i = 0;
    const char *str = mon_response_str_list[0];
    for (; i + 1 < buf_size; i++)
    {
        char c = str[state];
        buf[i] = c;
        if (!c)
            return -1;
        state++;
        buf_size--;
    }
    buf[i] = 0;
    return state;
}

void mon_add_response_fn(mon_response_fn fn)
{
    assert(mon_response_state < 0 && mon_response_pos < 0);
    mon_append_response(fn, NULL);
}

void mon_add_response_str(const char *str)
{
    assert(mon_response_state < 0 && mon_response_pos < 0);
    mon_append_response(mon_str_response, str);
}

static bool mon_suspended(void)
{
    return main_active() ||
           // These may run the 6502 many times for a single
           // task so we can't depend on only main_active().
           ram_active() ||
           rom_active() ||
           fil_active();
}

void mon_task(void)
{
    if (mon_suspended())
        return;
    if (mon_response_state >= 0 || mon_response_pos >= 0)
    {
        while (response_buf[mon_response_pos] && com_putchar_ready())
            putchar(response_buf[mon_response_pos++]);
        if (!response_buf[mon_response_pos])
            mon_response_pos = -1;
        if (mon_response_pos == -1 && mon_response_state >= 0)
        {
            mon_response_pos = 0;
            response_buf[0] = 0;
            mon_response_state = (mon_response_fn_list[0])(
                response_buf, RESPONSE_BUF_SIZE, mon_response_state);
            if (mon_response_state < 0)
                mon_response_state = mon_next_response();
        }
    }
    else if (mon_needs_prompt)
    {
        if (mon_needs_newline)
            mon_add_response_str(STR_MON_PROMPT_NEWLINE);
        else
            mon_add_response_str(STR_MON_PROMPT);
        mon_needs_prompt = false;
        mon_needs_newline = false;
        mon_needs_read_line = true;
    }
    else if (mon_needs_read_line)
    {
        mon_needs_read_line = false;
        rln_read_line(0, mon_enter, 256, 0);
    }
}

void mon_break(void)
{
    mon_needs_prompt = true;
    mon_needs_newline = true;
}
