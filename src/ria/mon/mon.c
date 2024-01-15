/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "str.h"
#include "api/std.h"
#include "mon/fil.h"
#include "mon/hlp.h"
#include "mon/mon.h"
#include "mon/ram.h"
#include "mon/rom.h"
#include "mon/set.h"
#include "sys/com.h"
#include "sys/mem.h"
#include "sys/sys.h"
#include "sys/vga.h"
#include "pico/stdlib.h"
#include <stdio.h>

static bool needs_newline = true;
static bool needs_prompt = true;

typedef void (*mon_function)(const char *, size_t);
static struct
{
    size_t cmd_len;
    const char *const cmd;
    mon_function func;
} const COMMANDS[] = {
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
static mon_function mon_command_lookup(const char **buf, uint8_t buflen)
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
        if (char_is_hex(ch))
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
    if (cmd_len == 2 && !strnicmp(cmd, "cd", cmd_len))
        is_not_addr = true;
    // address command
    if (is_maybe_addr && !is_not_addr)
    {
        *buf = cmd;
        return ram_mon_address;
    }
    // 0:-9: is chdrive
    if (cmd_len == 2 && cmd[1] == ':' && cmd[0] >= '0' && cmd[0] <= '9')
    {
        *buf = cmd;
        return fil_mon_chdrive;
    }
    *buf += i;
    for (i = 0; i < COMMANDS_COUNT; i++)
    {
        if (cmd_len == COMMANDS[i].cmd_len)
            if (!strnicmp(cmd, COMMANDS[i].cmd, cmd_len))
                return COMMANDS[i].func;
    }
    return NULL;
}

bool mon_command_exists(const char *buf, uint8_t buflen)
{
    return !!mon_command_lookup(&buf, buflen);
}

static void mon_enter(bool timeout, const char *buf, size_t length)
{
    (void)timeout;
    assert(!timeout);
    needs_prompt = true;
    const char *args = buf;
    mon_function func = mon_command_lookup(&args, length);
    if (!func)
    {
        if (!rom_load(buf, length))
            for (const char *b = buf; b < args; b++)
                if (b[0] != ' ')
                {
                    printf("?unknown command\n");
                    break;
                }
        return;
    }
    size_t args_len = length - (args - buf);
    func(args, args_len);
}

// Anything that suspends the monitor.
static bool mon_suspended(void)
{
    return main_active() ||
           ram_active() ||
           rom_active() ||
           vga_active() ||
           fil_active() ||
           std_active();
}

void mon_task(void)
{
    if (needs_prompt && !mon_suspended())
    {
        printf("\30\33[0m");
        if (needs_newline)
            putchar('\n');
        putchar(']');
        needs_prompt = false;
        needs_newline = false;
        com_read_line(0, mon_enter, 256, 0);
    }
}

void mon_reset(void)
{
    needs_prompt = true;
    needs_newline = true;
}
