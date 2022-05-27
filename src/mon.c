/*
 * Copyright (c) 2022 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mon.h"
#include "ansi.h"
#include "vga.h"
#include <stdio.h>
#include "pico/stdlib.h"

#define BUFLEN 80
static uint8_t mon_buf[BUFLEN];
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

static int to_int(uint8_t ch)
{
    if ((ch >= '0') && (ch <= '9'))
        return ch - '0';
    if (ch - 'A' < 6)
        return ch - 'A' + 10;
    if (ch - 'a' < 6)
        return ch - 'a' + 10;
}

// Hello World: 100f0 48 65 6c 6c 6f 20 57 6f 72 6c 64 21
static void mon_enter()
{
    uint32_t addr = 0x80000000;
    int i;
    for (i = 0; i < mon_buflen; i++)
    {
        uint8_t ch = mon_buf[i];
        if (is_hex(ch))
            addr = addr * 16 + to_int(ch);
        else if (ch == ' ' && addr == 0x80000000)
            continue;
        else
            break;
    }
    for (; i < mon_buflen; i++)
        if (mon_buf[i] != ' ')
            break;
    if (addr > 0x1FFFF)
    {
        printf("\n?Syntax error, invalid address\n");
        mon_buflen = mon_bufpos = 0;
        return;
    }
    if (i == mon_buflen)
    {
        mon_buf[mon_buflen] = 0;
        printf("\n%04X", addr);
        while (true)
        {
            if (addr < 0x10000)
                printf(" ??");
            else
                printf(" %02X", vga_memory[addr - 0x10000]);
            if (!(++addr & 0xF))
                break;
        }
        printf("\n");
        mon_buflen = mon_bufpos = 0;
        return;
    }
    uint32_t data = 0x80000000;
    for (; i < mon_buflen; i++)
    {
        uint8_t ch = mon_buf[i];
        if (is_hex(ch))
            data = data * 16 + to_int(ch);

        if (ch == ' ' || (i == mon_buflen - 1 && data < 0x100))
        {
            if (addr >= 0x10000 && data < 0x100)
                vga_memory[addr++ - 0x10000] = data;
            data = 0x80000000;
        }
        else if (!is_hex(ch))
        {
            printf("\n?Syntax error, invalid data");
            break;
        }
    }
    printf("\n");
    mon_buflen = mon_bufpos = 0;
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
    if (ch == '\b' || ch == 127)
        mon_backspace();
    else if (ch == '\r')
        mon_enter();
    else if (ch == '\33')
        mon_ansi_state = ansi_state_Fe;
    else if (ch >= 32 && ch < 127 && mon_bufpos < BUFLEN - 1)
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
    if (ch == ANSI_CANCEL) // cancel
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
