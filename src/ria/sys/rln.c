/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sys/rln.h"
#include <pico/stdlib.h>
#include <stdio.h>

#if defined(DEBUG_RIA_SYS) || defined(DEBUG_RIA_SYS_RLN)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define RLN_BUF_SIZE 256
#define RLN_CSI_PARAM_MAX_LEN 16

typedef enum
{
    ansi_state_C0,
    ansi_state_Fe,
    ansi_state_SS2,
    ansi_state_SS3,
    ansi_state_CSI,
    ansi_state_CSI_private,
} rln_ansi_state_t;

static char rln_buf[RLN_BUF_SIZE];
static rln_read_callback_t rln_callback;
static uint8_t *rln_binary_buf;
static absolute_time_t rln_timer;
static uint32_t rln_timeout_ms;
static size_t rln_bufsize;
static size_t rln_buflen;
static size_t rln_bufpos;
static rln_ansi_state_t rln_ansi_state;
static uint16_t rln_csi_param[RLN_CSI_PARAM_MAX_LEN];
static uint8_t rln_csi_param_count;
static uint32_t rln_ctrl_bits;

volatile size_t rln_tx_tail;
volatile size_t rln_tx_head;
volatile uint8_t rln_tx_buf[32];
#define RLN_TX_BUF(pos) rln_tx_buf[(pos) & 0x1F]

static void rln_line_home(void)
{
    if (rln_bufpos)
        printf("\33[%dD", rln_bufpos);
    rln_bufpos = 0;
}

static void rln_line_end(void)
{
    if (rln_bufpos != rln_buflen)
        printf("\33[%dC", rln_buflen - rln_bufpos);
    rln_bufpos = rln_buflen;
}

static void rln_line_forward_word(void)
{
    int count = 0;
    if (rln_bufpos < rln_buflen)
        while (true)
        {
            count++;
            if (++rln_bufpos >= rln_buflen)
                break;
            if (rln_buf[rln_bufpos] == ' ' && rln_buf[rln_bufpos - 1] != ' ')
                break;
        }
    if (count)
        printf("\33[%dC", count);
}

static void rln_line_forward(void)
{
    uint16_t count = rln_csi_param[0];
    if (count < 1)
        count = 1;
    if (rln_csi_param_count > 1 && !!(rln_csi_param[1] - 1))
        return rln_line_forward_word();
    if (count > rln_buflen - rln_bufpos)
        count = rln_buflen - rln_bufpos;
    if (!count)
        return;
    rln_bufpos += count;
    printf("\33[%dC", count);
}

static void rln_line_forward_1(void)
{
    rln_csi_param_count = 1;
    rln_csi_param[0] = 1;
    rln_line_forward();
}

static void rln_line_backward_word(void)
{
    int count = 0;
    if (rln_bufpos)
        while (true)
        {
            count++;
            if (!--rln_bufpos)
                break;
            if (rln_buf[rln_bufpos] != ' ' && rln_buf[rln_bufpos - 1] == ' ')
                break;
        }
    if (count)
        printf("\33[%dD", count);
}

static void rln_line_backward(void)
{
    uint16_t count = rln_csi_param[0];
    if (count < 1)
        count = 1;
    if (rln_csi_param_count > 1 && !!(rln_csi_param[1] - 1))
        return rln_line_backward_word();
    if (count > rln_bufpos)
        count = rln_bufpos;
    if (!count)
        return;
    rln_bufpos -= count;
    printf("\33[%dD", count);
}

static void rln_line_backward_1(void)
{
    rln_csi_param_count = 1;
    rln_csi_param[0] = 1;
    rln_line_backward();
}

static void rln_line_delete(void)
{
    if (!rln_buflen || rln_bufpos == rln_buflen)
        return;
    printf("\33[P");
    rln_buflen--;
    for (uint8_t i = rln_bufpos; i < rln_buflen; i++)
        rln_buf[i] = rln_buf[i + 1];
}

static void rln_line_backspace(void)
{
    if (!rln_bufpos)
        return;
    printf("\b\33[P");
    rln_buflen--;
    for (uint8_t i = --rln_bufpos; i < rln_buflen; i++)
        rln_buf[i] = rln_buf[i + 1];
}

static void rln_line_insert(char ch)
{
    if (ch < 32 || rln_buflen >= rln_bufsize - 1)
        return;
    for (size_t i = rln_buflen; i > rln_bufpos; i--)
        rln_buf[i] = rln_buf[i - 1];
    rln_buflen++;
    rln_buf[rln_bufpos] = ch;
    for (size_t i = rln_bufpos; i < rln_buflen; i++)
        putchar(rln_buf[i]);
    rln_bufpos++;
    if (rln_buflen - rln_bufpos)
        printf("\33[%dD", rln_buflen - rln_bufpos);
}

static void rln_line_state_C0(char ch)
{
    if (rln_ctrl_bits & (1 << ch))
    {
        printf("\n");
        rln_buf[0] = ch;
        rln_buf[1] = 0;
        rln_buflen = 1;
        rln_read_callback_t cc = rln_callback;
        rln_callback = NULL;
        cc(false, rln_buf, rln_buflen);
    }
    else if (ch == '\r')
    {
        printf("\n");
        rln_buf[rln_buflen] = 0;
        rln_read_callback_t cc = rln_callback;
        rln_callback = NULL;
        cc(false, rln_buf, rln_buflen);
    }
    else if (ch == '\33')
        rln_ansi_state = ansi_state_Fe;
    else if (ch == '\b' || ch == 127)
        rln_line_backspace();
    else if (ch == 1) // ctrl-a
        rln_line_home();
    else if (ch == 2) // ctrl-b
        rln_line_backward_1();
    else if (ch == 5) // ctrl-e
        rln_line_end();
    else if (ch == 6) // ctrl-f
        rln_line_forward_1();
    else
        rln_line_insert(ch);
}

static void rln_line_state_Fe(char ch)
{
    if (ch == '[')
    {
        rln_ansi_state = ansi_state_CSI;
        rln_csi_param_count = 0;
        rln_csi_param[0] = 0;
    }
    else if (ch == 'b' || ch == 2)
    {
        rln_ansi_state = ansi_state_C0;
        rln_line_backward_word();
    }
    else if (ch == 'f' || ch == 6)
    {
        rln_ansi_state = ansi_state_C0;
        rln_line_forward_word();
    }
    else if (ch == 'N')
        rln_ansi_state = ansi_state_SS2;
    else if (ch == 'O')
        rln_ansi_state = ansi_state_SS3;
    else
    {
        rln_ansi_state = ansi_state_C0;
        if (ch == 127)
            rln_line_delete();
    }
}

static void rln_line_state_SS2(char ch)
{
    (void)ch;
    rln_ansi_state = ansi_state_C0;
}

static void rln_line_state_SS3(char ch)
{
    rln_ansi_state = ansi_state_C0;
    if (ch == 'F')
        rln_line_end();
    else if (ch == 'H')
        rln_line_home();
}

static void rln_line_state_CSI(char ch)
{
    // Silently discard overflow parameters but still count to + 1.
    if (ch >= '0' && ch <= '9')
    {
        if (rln_csi_param_count < RLN_CSI_PARAM_MAX_LEN)
        {
            rln_csi_param[rln_csi_param_count] *= 10;
            rln_csi_param[rln_csi_param_count] += ch - '0';
        }
        return;
    }
    if (ch == ';' || ch == ':')
    {
        if (++rln_csi_param_count < RLN_CSI_PARAM_MAX_LEN)
            rln_csi_param[rln_csi_param_count] = 0;
        else
            rln_csi_param_count = RLN_CSI_PARAM_MAX_LEN;
        return;
    }
    if (ch == '<' || ch == '=' || ch == '>' || ch == '?')
    {
        rln_ansi_state = ansi_state_CSI_private;
        return;
    }
    if (rln_ansi_state == ansi_state_CSI_private)
    {
        rln_ansi_state = ansi_state_C0;
        return;
    }
    rln_ansi_state = ansi_state_C0;
    if (++rln_csi_param_count > RLN_CSI_PARAM_MAX_LEN)
        rln_csi_param_count = RLN_CSI_PARAM_MAX_LEN;
    if (ch == 'C')
        rln_line_forward();
    else if (ch == 'D')
        rln_line_backward();
    else if (ch == 'F')
        rln_line_end();
    else if (ch == 'H')
        rln_line_home();
    else if (ch == 'b' || ch == 2)
        rln_line_backward_word();
    else if (ch == 'f' || ch == 6)
        rln_line_forward_word();
    else if (ch == '~')
        switch (rln_csi_param[0])
        {
        case 1:
        case 7:
            return rln_line_home();
        case 4:
        case 8:
            return rln_line_end();
        case 3:
            return rln_line_delete();
        }
}

static void rln_line_rx(uint8_t ch)
{
    if (ch == '\30')
        rln_ansi_state = ansi_state_C0;
    else
        switch (rln_ansi_state)
        {
        case ansi_state_C0:
            rln_line_state_C0(ch);
            break;
        case ansi_state_Fe:
            rln_line_state_Fe(ch);
            break;
        case ansi_state_SS2:
            rln_line_state_SS2(ch);
            break;
        case ansi_state_SS3:
            rln_line_state_SS3(ch);
            break;
        case ansi_state_CSI:
        case ansi_state_CSI_private:
            rln_line_state_CSI(ch);
            break;
        }
}

static void rln_binary_rx(uint8_t ch)
{
    rln_binary_buf[rln_buflen] = ch;
    if (++rln_buflen == rln_bufsize)
    {
        rln_read_callback_t cc = rln_callback;
        rln_callback = NULL;
        cc(false, (char *)rln_binary_buf, rln_buflen);
        rln_binary_buf = NULL;
    }
}

void rln_read_binary(uint32_t timeout_ms, rln_read_callback_t callback, uint8_t *buf, size_t size)
{
    rln_binary_buf = buf;
    rln_bufsize = size;
    rln_buflen = 0;
    rln_timeout_ms = timeout_ms;
    rln_timer = make_timeout_time_ms(rln_timeout_ms);
    rln_callback = callback;
}

void rln_read_line(uint32_t timeout_ms, rln_read_callback_t callback, size_t size, uint32_t ctrl_bits)
{
    rln_bufsize = size;
    if (rln_bufsize > RLN_BUF_SIZE)
        rln_bufsize = RLN_BUF_SIZE;
    rln_buflen = 0;
    rln_bufpos = 0;
    rln_ansi_state = ansi_state_C0;
    rln_timeout_ms = timeout_ms;
    rln_timer = make_timeout_time_ms(rln_timeout_ms);
    rln_callback = callback;
    rln_ctrl_bits = ctrl_bits;
}

void rln_task(void)
{
    if (rln_callback)
    {
        int ch = stdio_getchar_timeout_us(0);
        if (ch != PICO_ERROR_TIMEOUT)
            rln_timer = make_timeout_time_ms(rln_timeout_ms);
        while (rln_callback && ch != PICO_ERROR_TIMEOUT)
        {
            if (rln_binary_buf)
                rln_binary_rx(ch);
            else
                rln_line_rx(ch);
            ch = stdio_getchar_timeout_us(0);
        }
        if (rln_timeout_ms && absolute_time_diff_us(get_absolute_time(), rln_timer) < 0)
        {
            rln_read_callback_t cc = rln_callback;
            rln_callback = NULL;
            rln_binary_buf = NULL;
            cc(true, NULL, 0);
        }
    }
}

void rln_break(void)
{
    rln_callback = NULL;
    rln_binary_buf = NULL;
}
