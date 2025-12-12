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
#include <strings.h>
#include <ctype.h>

#if defined(DEBUG_RIA_MON) || defined(DEBUG_RIA_MON_MON)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

static int (*mon_response_fn)(char *, size_t, int);
static int mon_response_state = -1;
static int mon_response_pos = -1;
static const char *mon_response_str;
static bool mon_needs_newline = true;
static bool mon_needs_prompt = true;

typedef void (*mon_function)(const char *, size_t);
static struct
{
    size_t cmd_len;
    const char *const cmd;
    mon_function func;
} const __in_flash("mon_commands") COMMANDS[] = {
    {4, "help", hlp_mon_help},
    {1, "h", hlp_mon_help},
    {1, "?", hlp_mon_help},
    {6, "status", sys_mon_status},
    {3, "set", set_mon_set},
    {2, "ls", fil_mon_ls},
    {3, "dir", fil_mon_ls},
    {2, "cd", fil_mon_chdir},
    {5, "chdir", fil_mon_chdir},
    {5, "mkdir", fil_mon_mkdir},
    {4, "load", rom_mon_load},
    {4, "info", rom_mon_info},
    {7, "install", rom_mon_install},
    {6, "remove", rom_mon_remove},
    {6, "reboot", sys_mon_reboot},
    {5, "reset", sys_mon_reset},
    {6, "upload", fil_mon_upload},
    {6, "unlink", fil_mon_unlink},
    {6, "binary", ram_mon_binary},
};
static const size_t COMMANDS_COUNT = sizeof COMMANDS / sizeof *COMMANDS;

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
    for (i = 0; i < COMMANDS_COUNT; i++)
    {
        if (cmd_len == COMMANDS[i].cmd_len)
            if (!strncasecmp(cmd, COMMANDS[i].cmd, cmd_len))
                return COMMANDS[i].func;
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
    for (const char *b = buf; b < args; b++)
        if (b[0] != ' ')
        {
            printf("?unknown command\n");
            break;
        }
}

static int mon_str_response(char *buf, size_t buf_size, int state)
{
    (void)state;
    snprintf(buf, buf_size, mon_response_str, state);
    return -1;
}

void mon_set_response_fn(int (*fn)(char *, size_t, int))
{
    assert(mon_response_state < 0 && mon_response_pos < 0);
    mon_response_fn = fn;
    mon_response_state = 0;
}

void mon_set_response_str(const char *str)
{
    assert(mon_response_state < 0 && mon_response_pos < 0);
    mon_response_fn = mon_str_response;
    mon_response_state = 0;
    mon_response_str = str;
}

void mon_set_response_str_int(const char *str, int i)
{
    assert(mon_response_state < 0 && mon_response_pos < 0);
    assert(i >= 0);
    mon_response_fn = mon_str_response;
    mon_response_state = i;
    mon_response_str = str;
}

// Anything that suspends the monitor.
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
            mon_response_state = mon_response_fn(
                response_buf, RESPONSE_BUF_SIZE, mon_response_state);
        }
    }
    else if (mon_needs_prompt && !mon_suspended())
    {
        printf("\30\33[0m\33[?25h");
        if (mon_needs_newline)
            putchar('\n');
        putchar(']');
        mon_needs_prompt = false;
        mon_needs_newline = false;
        rln_read_line(0, mon_enter, 256, 0);
    }
}

void mon_break(void)
{
    mon_needs_prompt = true;
    mon_needs_newline = true;
}
