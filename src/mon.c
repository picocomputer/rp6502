/*
 * Copyright (c) 2022 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mon.h"
#include "ansi.h"
#include "ria.h"
#include <stdio.h>
#include "pico/stdlib.h"

#define MON_BUF_SIZE 80
static uint8_t mon_buf[MON_BUF_SIZE];
static uint8_t mon_buflen = 0;
static uint8_t mon_bufpos = 0;
static ansi_state_t mon_ansi_state = ansi_state_C0;
static int mon_ansi_param;

static bool is_hex(uint8_t ch)
{
    return ((ch >= '0') && (ch <= '9')) ||
           ((ch >= 'A') && (ch <= 'F')) ||
           ((ch >= 'a') && (ch <= 'f'));
}

static int hex_to_int(uint8_t ch)
{
    if ((ch >= '0') && (ch <= '9'))
        return ch - '0';
    if (ch - 'A' < 6)
        return ch - 'A' + 10;
    if (ch - 'a' < 6)
        return ch - 'a' + 10;
}

static int strnicmp(const char *string1, const char *string2, int n)
{
    while (n--)
    {
        if (!*string1 && !*string2)
            return 0;
        int ch1 = *string1;
        int ch2 = *string2;
        if (ch1 >= 'a' && ch1 <= 'z')
            ch1 -= 32;
        if (ch2 >= 'a' && ch2 <= 'z')
            ch2 -= 32;
        int rc = ch1 - ch2;
        if (rc)
            return rc;
        string1++;
        string2++;
    }
    return 0;
}

// Commands that start with a hex address. Read or write memory.
static void cmd_address(uint32_t addr, char *args, size_t len)
{
    // TODO rework for RIA
    if (addr > 0x1FFFF)
    {
        printf("?invalid address\n");
        mon_buflen = mon_bufpos = 0;
        return;
    }
    if (!len)
    {
        mon_buf[mon_buflen] = 0;
        printf("%04X:", addr);
        while (true)
        {
            if (addr < 0x10000)
                printf(" ??");
            else
                printf(" %02X", vram[addr - 0x10000]);
            if (!(++addr & 0xF))
                break;
        }
        printf("\n");
        return;
    }
    uint32_t data = 0x80000000;
    for (size_t i = 0; i < len; i++)
    {
        uint8_t ch = args[i];
        if (is_hex(ch))
            data = data * 16 + hex_to_int(ch);
        else if (ch != ' ')
        {
            printf("?invalid data character\n");
            break;
        }
        if (ch == ' ' || i == len - 1)
        {
            if (data < 0x100)
            {
                if (addr >= 0x10000 && data < 0x100)
                    vram[addr++ - 0x10000] = data;
                data = 0x80000000;
            }
            else
            {
                printf("?invalid data value\n");
                break;
            }
            for (; i + 1 < len; i++)
            {
                if (args[i + 1] != ' ')
                    break;
            }
        }
    }
}

static void cmd_speed(uint8_t *args, size_t len)
{
    printf("TODO speed\n");
}

static void cmd_reset(uint8_t *args, size_t len)
{
    printf("TODO reset\n");
}

static void cmd_status(uint8_t *args, size_t len)
{
    printf("TODO status\n");
}

static void cmd_jmp(uint8_t *args, size_t len)
{
    printf("TODO jmp\n");
}

static void cmd_help(uint8_t *args, size_t len)
{
    printf("TODO help\n");
}

struct
{
    size_t cmd_len;
    const char *cmd;
    void (*func)(uint8_t *, size_t);
} const COMMANDS[] = {
    {5, "speed", cmd_speed},
    {5, "reset", cmd_reset},
    {5, "status", cmd_status},
    {3, "jmp", cmd_jmp},
    {4, "help", cmd_help},
};
const size_t COMMANDS_COUNT = sizeof COMMANDS / sizeof *COMMANDS;

static void mon_enter()
{
    // find the cmd and args
    size_t i;
    for (i = 0; i < mon_buflen; i++)
    {
        if (mon_buf[i] != ' ')
            break;
    }
    uint8_t *cmd = mon_buf + i;
    uint32_t addr = 0;
    bool is_maybe_addr = false;
    bool is_not_addr = false;
    for (; i < mon_buflen; i++)
    {
        uint8_t ch = mon_buf[i];
        if (is_hex(ch))
        {
            is_maybe_addr = true;
            addr = addr * 16 + hex_to_int(ch);
        }
        else if (is_maybe_addr && !is_not_addr && ch == ':')
        {
            // optional colon "0000: 00 00"
            i++;
            break;
        }
        else if (ch == ' ')
            break;
        else
            is_not_addr = true;
    }
    size_t cmd_len = mon_buf + i - cmd;
    for (; i < mon_buflen; i++)
    {
        if (mon_buf[i] != ' ')
            break;
    }
    char *args = mon_buf + i;
    size_t args_len = mon_buflen - i;

    // dispatch command
    if (is_maybe_addr && !is_not_addr)
        return cmd_address(addr, args, args_len);
    for (i = 0; i < COMMANDS_COUNT; i++)
    {
        if (cmd_len == COMMANDS[i].cmd_len)
            if (!strnicmp(cmd, COMMANDS[i].cmd, cmd_len))
                return COMMANDS[i].func(args, args_len);
    }
    if (cmd_len)
        printf("?unknown command\n");
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
        mon_buflen = mon_bufpos = 0;
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

void mon_task()
{
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
