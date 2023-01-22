/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mon.h"
#include "cmd.h"
#include "rom.h"
#include "vga/ansi.h"
#include "ria.h"
#include <stdio.h>
#include "pico/stdlib.h"

#define MON_BUF_SIZE 79
static char mon_buf[MON_BUF_SIZE];
static uint8_t mon_buflen = 0;
static uint8_t mon_bufpos = 0;
static ansi_state_t mon_ansi_state = ansi_state_C0;
static int mon_ansi_param;
static bool needs_prompt = true;

static void mon_enter()
{
    mon_buf[mon_buflen] = 0;
    cmd_dispatch(mon_buf, mon_buflen);
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

void mon_task()
{
    if (ria_is_active() || cmd_is_active() || rom_is_active())
    {
        needs_prompt = true;
        return;
    }

    if (needs_prompt)
    {
        needs_prompt = false;
        putchar(cmd_prompt());
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
