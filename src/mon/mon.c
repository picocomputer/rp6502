/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mon.h"
#include "cmd.h"
#include "hlp.h"
#include "fil.h"
#include "rom.h"
#include "str.h"
#include "mem/mbuf.h"
#include "vga/ansi.h"
#include "ria/ria.h"
#include <stdio.h>
#include "pico/stdlib.h"

#define MON_BUF_SIZE 79
static char mon_buf[MON_BUF_SIZE];
static uint8_t mon_buflen = 0;
static uint8_t mon_bufpos = 0;
static ansi_state_t mon_ansi_state = ansi_state_C0;
static int mon_ansi_param;
static bool needs_prompt = true;

typedef void (*cmd_function)(const char *, size_t);
static struct
{
    size_t cmd_len;
    const char *const cmd;
    cmd_function func;
} const COMMANDS[] = {
    {4, "help", hlp_help},
    {1, "h", hlp_help},
    {1, "?", hlp_help},
    {6, "status", cmd_status},
    {4, "caps", cmd_caps},
    {4, "phi2", cmd_phi2},
    {4, "resb", cmd_resb},
    {2, "ls", fil_ls},
    {3, "dir", fil_ls},
    {2, "cd", fil_chdir},
    {4, "load", rom_load},
    {7, "install", rom_install},
    {6, "remove", rom_remove},
    // {4, "boot", rom_boot},
    // {6, "reboot", rom_reboot},
    {5, "reset", cmd_start},
    {6, "upload", fil_upload},
    {6, "unlink", fil_unlink},
    {6, "binary", cmd_binary},
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
        return cmd_address;
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

static void mon_command_dispatch(const char *buf, uint8_t buflen)
{
    const char *args = buf;
    cmd_function func = mon_command_lookup(&args, buflen);
    if (!func)
    {
        if (!rom_load_lfs(buf, buflen))
            for (; buf < args; buf++)
                if (buf[0] != ' ')
                {
                    printf("?unknown command\n");
                    break;
                }
        return;
    }
    size_t args_len = buflen - (args - buf);
    func(args, args_len);
}

static void mon_enter()
{
    mon_buf[mon_buflen] = 0;
    if (fil_is_prompting())
        fil_command_dispatch(mon_buf, mon_buflen);
    else
        mon_command_dispatch(mon_buf, mon_buflen);
    mon_reset();
}

static void mon_forward(int count)
{
    if (count > mon_buflen - mon_bufpos)
        count = mon_buflen - mon_bufpos;
    if (!count)
        return;
    mon_bufpos += count;
    // clang-format off
    printf(ANSI_FORWARD(%d), count);
    // clang-format on
}

static void mon_backward(int count)
{
    if (count > mon_bufpos)
        count = mon_bufpos;
    if (!count)
        return;
    mon_bufpos -= count;
    // clang-format off
    printf(ANSI_BACKWARD(%d), count);
    // clang-format on
}

static void mon_delete()
{
    if (!mon_buflen || mon_bufpos == mon_buflen)
        return;
    printf(ANSI_DELETE(1));
    mon_buflen--;
    for (uint8_t i = mon_bufpos; i < mon_buflen; i++)
        mon_buf[i] = mon_buf[i + 1];
}

static void mon_backspace()
{
    if (!mon_bufpos)
        return;
    printf("\b" ANSI_DELETE(1));
    mon_buflen--;
    for (uint8_t i = --mon_bufpos; i < mon_buflen; i++)
        mon_buf[i] = mon_buf[i + 1];
}

static void mon_state_C0(char ch)
{
    if (ch == '\33')
        mon_ansi_state = ansi_state_Fe;
    else if (ch == '\b' || ch == 127)
        mon_backspace();
    else if (ch == '\r')
    {
        printf("\n");
        mon_enter();
    }
    else if (ch >= 32 && ch < 127 && mon_bufpos < MON_BUF_SIZE - 1)
    {
        putchar(ch);
        mon_buf[mon_bufpos] = ch;
        if (++mon_bufpos > mon_buflen)
            mon_buflen = mon_bufpos;
    }
}

static void mon_state_Fe(char ch)
{
    if (ch == '[')
    {
        mon_ansi_state = ansi_state_CSI;
        mon_ansi_param = -1;
    }
    else if (ch == 'O')
    {
        mon_ansi_state = ansi_state_SS3;
    }
    else
    {
        mon_ansi_state = ansi_state_C0;
        mon_delete();
    }
}

static void mon_state_CSI(char ch)
{
    if (ch >= '0' && ch <= '9')
    {
        if (mon_ansi_param < 0)
        {
            mon_ansi_param = ch - '0';
        }
        else
        {
            mon_ansi_param *= 10;
            mon_ansi_param += ch - '0';
        }
        return;
    }
    if (ch == ';')
        return;
    mon_ansi_state = ansi_state_C0;
    if (mon_ansi_param < 0)
        mon_ansi_param = -mon_ansi_param;
    if (ch == 'C')
        mon_forward(mon_ansi_param);
    else if (ch == 'D')
        mon_backward(mon_ansi_param);
    else if (ch == '~' && mon_ansi_param == 3)
        mon_delete();
}

static void mon_rx_binary()
{
    int ch = getchar_timeout_us(0);
    if (ch != PICO_ERROR_TIMEOUT)
    {
        while (ch != PICO_ERROR_TIMEOUT)
        {
            mbuf[mbuf_len++] = ch;
            if (fil_is_rx_binary() && fil_rx_handler())
                return;
            if (cmd_is_rx_binary() && cmd_rx_handler())
                return;
            ch = getchar_timeout_us(0);
        }
        if (fil_is_rx_binary())
            fil_keep_alive();
        if (cmd_is_rx_binary())
            cmd_keep_alive();
    }
}

void mon_task()
{
    if (ria_is_active() || cmd_is_active() || rom_is_active())
    {
        needs_prompt = true;
        return;
    }
    if (cmd_is_rx_binary() || fil_is_rx_binary())
        return mon_rx_binary();
    if (needs_prompt)
    {
        needs_prompt = false;
        putchar(fil_is_prompting() ? '}' : ']');
    }
    int ch = getchar_timeout_us(0);
    if (ch == ANSI_CANCEL)
        mon_ansi_state = ansi_state_C0;
    else if (ch != PICO_ERROR_TIMEOUT)
        switch (mon_ansi_state)
        {
        case ansi_state_C0:
            mon_state_C0(ch);
            break;
        case ansi_state_Fe:
            mon_state_Fe(ch);
            break;
        case ansi_state_SS3:
            // all SS3 is nop
            mon_ansi_state = ansi_state_C0;
            break;
        case ansi_state_CSI:
            mon_state_CSI(ch);
            break;
        }
}

void mon_reset()
{
    mon_ansi_state = ansi_state_C0;
    mon_buflen = 0;
    mon_bufpos = 0;
    needs_prompt = true;
}
