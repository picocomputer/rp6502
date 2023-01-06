/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mon.h"
#include "cmd.h"
#include "msc.h"
#include "ansi.h"
#include "ria.h"
#include "ria_action.h"
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"

#define MON_BUF_SIZE 80
static uint8_t mon_buf[MON_BUF_SIZE];
static uint8_t mon_buflen = 0;
static uint8_t mon_bufpos = 0;
static ansi_state_t mon_ansi_state = ansi_state_C0;
static int mon_ansi_param;
static volatile enum state {
    idle,
    read,
    write,
    verify,
} mon_state = idle;
static uint8_t *mon_rw_data;
static uint32_t mon_rw_addr;
static size_t mon_rw_len;

static void mon_enter()
{

    mon_buf[mon_buflen] = 0;
    cmd_dispatch(mon_buf, mon_buflen);
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

static void mon_char(int ch)
{
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

void mon_task()
{
    if (ria_is_active())
        return;

    if (mon_state == idle)
        return mon_char(getchar_timeout_us(0));

    int32_t result = ria_action_result();
    if (result != -1)
    {
        mon_state = idle;
        if (result == -2)
            printf("?watchdog timeout\n");
        else
            printf("?verify failed at $%04X\n", result);
    }

    switch (mon_state)
    {
    case read:
        mon_state = idle;
        //TODO move to cmd
        printf("%04X:", mon_rw_addr);
        for (size_t i = 0; i < mon_rw_len; i++)
        {
            printf(" %02X", mon_rw_data[i]);
        }
        printf("\n");
        break;
    case write:
        mon_state = verify;
        ria_action_ram_verify(mon_rw_addr, mon_rw_data, mon_rw_len);
        break;
    case verify:
        mon_state = idle;
        break;
    }
}

void mon_read(uint16_t addr, uint8_t *data, uint16_t len)
{
    mon_state = read;
    mon_rw_addr = addr;
    mon_rw_data = data;
    mon_rw_len = len;
    ria_action_ram_read(addr, data, len);
}

void mon_write(uint16_t addr, uint8_t *data, uint16_t len)
{
    mon_state = write;
    mon_rw_addr = addr;
    mon_rw_data = data;
    mon_rw_len = len;
    ria_action_ram_write(addr, data, len);
}

void mon_reset()
{
    mon_state == idle;
    mon_ansi_state = ansi_state_C0;
    mon_buflen = 0;
    mon_bufpos = 0;
}
