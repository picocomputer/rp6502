/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "sys/com.h"
#include "sys/cpu.h"
#include "sys/ria.h"
#include "vga/ansi.h"
#include "pico/stdlib.h"
#include <stdio.h>

static com_read_callback_t com_callback;
uint8_t *com_binary_buf;
char *com_line_buf;
static absolute_time_t com_timer;
uint32_t com_timeout_ms;
size_t com_bufsize;
size_t com_buflen;
size_t com_bufpos;
ansi_state_t com_ansi_state;
int com_ansi_param;

void com_init()
{
    com_reclock();
}

void com_reset()
{
    com_callback = NULL;
    com_binary_buf = NULL;
    com_line_buf = NULL;
}

void com_flush()
{
    // flush every buffer
    while (getchar_timeout_us(0) >= 0)
        tight_loop_contents();
    while (!(uart_get_hw(COM_UART)->fr & UART_UARTFR_TXFE_BITS))
        tight_loop_contents();
}

void com_reclock()
{
    stdio_uart_init_full(COM_UART, COM_UART_BAUD_RATE,
                         COM_UART_TX_PIN, COM_UART_RX_PIN);
}

size_t com_write(char *ptr, size_t count)
{
    size_t bw = 0;
    for (; count && uart_is_writable(COM_UART); --count, bw++)
    {
        uint8_t ch = *(uint8_t *)ptr++;
        if (ch == '\n')
        {
            uart_putc_raw(COM_UART, '\r');
            uart_putc_raw(COM_UART, ch);
        }
        else
            uart_get_hw(COM_UART)->dr = ch;
    }
    return bw;
}

static void com_line_forward(size_t count)
{
    if (count > com_buflen - com_bufpos)
        count = com_buflen - com_bufpos;
    if (!count)
        return;
    com_bufpos += count;
    // clang-format off
    printf(ANSI_FORWARD(%d), count);
    // clang-format on
}

static void com_line_backward(size_t count)
{
    if (count > com_bufpos)
        count = com_bufpos;
    if (!count)
        return;
    com_bufpos -= count;
    // clang-format off
    printf(ANSI_BACKWARD(%d), count);
    // clang-format on
}

static void com_line_delete()
{
    if (!com_buflen || com_bufpos == com_buflen)
        return;
    printf(ANSI_DELETE(1));
    com_buflen--;
    for (uint8_t i = com_bufpos; i < com_buflen; i++)
        com_line_buf[i] = com_line_buf[i + 1];
}

static void com_line_backspace()
{
    if (!com_bufpos)
        return;
    printf("\b" ANSI_DELETE(1));
    com_buflen--;
    for (uint8_t i = --com_bufpos; i < com_buflen; i++)
        com_line_buf[i] = com_line_buf[i + 1];
}

static void com_lilne_state_C0(char ch)
{
    if (ch == '\33')
        com_ansi_state = ansi_state_Fe;
    else if (ch == '\b' || ch == 127)
        com_line_backspace();
    else if (ch == '\r')
    {
        printf("\n");
        com_line_buf[com_buflen] = 0;
        com_read_callback_t cc = com_callback;
        com_callback = NULL;
        com_line_buf = NULL;
        cc(false, com_buflen);
    }
    else if (ch >= 32 && ch < 127 && com_bufpos < com_bufsize - 1)
    {
        putchar(ch);
        com_line_buf[com_bufpos] = ch;
        if (++com_bufpos > com_buflen)
            com_buflen = com_bufpos;
    }
}

static void com_line_state_Fe(char ch)
{
    if (ch == '[')
    {
        com_ansi_state = ansi_state_CSI;
        com_ansi_param = -1;
    }
    else if (ch == 'O')
    {
        com_ansi_state = ansi_state_SS3;
    }
    else
    {
        com_ansi_state = ansi_state_C0;
        com_line_delete(gets);
    }
}

static void com_line_state_CSI(char ch)
{
    if (ch >= '0' && ch <= '9')
    {
        if (com_ansi_param < 0)
        {
            com_ansi_param = ch - '0';
        }
        else
        {
            com_ansi_param *= 10;
            com_ansi_param += ch - '0';
        }
        return;
    }
    if (ch == ';')
        return;
    com_ansi_state = ansi_state_C0;
    if (com_ansi_param < 0)
        com_ansi_param = -com_ansi_param;
    if (ch == 'C')
        com_line_forward(com_ansi_param);
    else if (ch == 'D')
        com_line_backward(com_ansi_param);
    else if (ch == '~' && com_ansi_param == 3)
        com_line_delete(gets);
}

void com_line_rx(uint8_t ch)
{
    if (ch == ANSI_CANCEL)
        com_ansi_state = ansi_state_C0;
    else
        switch (com_ansi_state)
        {
        case ansi_state_C0:
            com_lilne_state_C0(ch);
            break;
        case ansi_state_Fe:
            com_line_state_Fe(ch);
            break;
        case ansi_state_SS3:
            // all SS3 is nop
            com_ansi_state = ansi_state_C0;
            break;
        case ansi_state_CSI:
            com_line_state_CSI(ch);
            break;
        }
}

void com_binary_rx(uint8_t ch)
{
    com_binary_buf[com_buflen] = ch;
    if (++com_buflen == com_bufsize)
    {
        com_read_callback_t cc = com_callback;
        com_callback = NULL;
        com_binary_buf = NULL;
        cc(false, com_buflen);
    }
}

void com_read_binary(uint8_t *buf, size_t size, uint32_t timeout_ms, com_read_callback_t callback)
{
    assert(!com_line_buf);
    com_binary_buf = buf;
    com_bufsize = size;
    com_buflen = 0;
    com_timeout_ms = timeout_ms;
    com_timer = delayed_by_ms(get_absolute_time(), com_timeout_ms);
    com_callback = callback;
}

void com_read_line(char *buf, size_t size, uint32_t timeout_ms, com_read_callback_t callback)
{
    assert(!com_binary_buf);
    com_line_buf = buf;
    com_bufsize = size;
    com_buflen = 0;
    com_bufpos = 0;
    com_ansi_state = ansi_state_C0;
    com_timeout_ms = timeout_ms;
    com_timer = delayed_by_ms(get_absolute_time(), com_timeout_ms);
    com_callback = callback;
}

void com_task()
{
    // Detect UART breaks.
    static uint32_t break_detect = 0;
    uint32_t current_break = uart_get_hw(COM_UART)->rsr & UART_UARTRSR_BE_BITS;
    if (current_break)
        hw_clear_bits(&uart_get_hw(COM_UART)->rsr, UART_UARTRSR_BITS);
    else if (break_detect)
        main_break();
    break_detect = current_break;

    // Allow UART RX FIFO to fill during RIA actions.
    // At all other times the FIFO must be emptied to detect breaks.
    if (!ria_active())
    {
        if (com_callback && com_timeout_ms && absolute_time_diff_us(get_absolute_time(), com_timer) < 0)
        {
            com_read_callback_t cc = com_callback;
            com_callback = NULL;
            com_binary_buf = NULL;
            com_line_buf = NULL;
            cc(true, com_buflen);
        }
        else
        {
            int ch = getchar_timeout_us(0);
            while (ch != PICO_ERROR_TIMEOUT)
            {
                if (com_callback)
                {
                    com_timer = delayed_by_ms(get_absolute_time(), com_timeout_ms);
                    if (com_line_buf)
                        com_line_rx(ch);
                    else if (com_binary_buf)
                        com_binary_rx(ch);
                }
                else if (cpu_active())
                    cpu_com_rx(ch);
                if (ria_active())
                    break;
                ch = getchar_timeout_us(0);
            }
        }
    }
}
