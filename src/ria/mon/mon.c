/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "str.h"
#include "mon/fil.h"
#include "mon/hlp.h"
#include "mon/mon.h"
#include "mon/ram.h"
#include "mon/rom.h"
#include "mon/set.h"
#include "sys/com.h"
#include "sys/sys.h"
#include "pico/stdlib.h"
#include <stdio.h>

static bool needs_prompt = true;

typedef void (*cmd_function)(const char *, size_t);
static struct
{
    size_t cmd_len;
    const char *const cmd;
    cmd_function func;
} const COMMANDS[] = {
    {4, "help", hlp_dispatch},
    {1, "h", hlp_dispatch},
    {1, "?", hlp_dispatch},
    {6, "status", set_status},
    {3, "set", set_attr},
    {2, "ls", fil_ls},
    {3, "dir", fil_ls},
    {2, "cd", fil_chdir},
    {4, "load", rom_load_fat},
    {4, "info", rom_help_fat},
    {7, "install", rom_install},
    {6, "remove", rom_remove},
    {6, "reboot", sys_reboot},
    {5, "reset", sys_run_6502},
    {6, "upload", fil_upload},
    {6, "unlink", fil_unlink},
    {6, "binary", ram_binary},
};
static const size_t COMMANDS_COUNT = sizeof COMMANDS / sizeof *COMMANDS;

// Returns 0 if not found. Advances buf to start of args.
static cmd_function mon_command_lookup(const char **buf, uint8_t buflen)
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
        return ram_address;
    }
    // 0:-9: is chdrive
    if (cmd_len == 2 && cmd[1] == ':' && cmd[0] >= '0' && cmd[0] <= '9')
    {
        *buf = cmd;
        return fil_chdrive;
    }
    *buf += i;
    for (i = 0; i < COMMANDS_COUNT; i++)
    {
        if (cmd_len == COMMANDS[i].cmd_len)
            if (!strnicmp(cmd, COMMANDS[i].cmd, cmd_len))
                return COMMANDS[i].func;
    }
    return 0;
}

bool mon_command_exists(const char *buf, uint8_t buflen)
{
    return !!mon_command_lookup(&buf, buflen);
}

static void mon_enter(bool timeout, size_t length)
{
    assert(!timeout);
    needs_prompt = true;
    const char *args = com_buf;
    cmd_function func = mon_command_lookup(&args, length);
    if (!func)
    {
        if (!rom_load_lfs(com_buf, length))
            for (char *b = com_buf; b < args; b++)
                if (b[0] != ' ')
                {
                    printf("?unknown command\n");
                    break;
                }
        return;
    }
    size_t args_len = length - (args - com_buf);
    func(args, args_len);
}

// Anything that suspends the monitor.
static bool mon_suspended()
{
    return main_active() ||
           ram_active() ||
           rom_active() ||
           fil_active();
}

void mon_task()
{
    if (needs_prompt && !mon_suspended())
    {
        needs_prompt = false;
        putchar(']');
        com_read_line(com_buf, COM_BUF_SIZE, 0, mon_enter);
    }
}

void mon_reset()
{
    needs_prompt = true;
}
