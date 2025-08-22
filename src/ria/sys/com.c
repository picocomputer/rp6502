/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "api/api.h"
#include "sys/com.h"
#include "sys/cpu.h"
#include "sys/mem.h"
#include "sys/pix.h"
#include "sys/ria.h"
#include "sys/rln.h"
#include "sys/vga.h"
#include "hid/kbd.h"
#include "pico/stdlib.h"
#include "pico/stdio/driver.h"
#include <stdio.h>

#define COM_BUF_SIZE 256
#define COM_CSI_PARAM_MAX_LEN 16

static stdio_driver_t com_stdio_app;

volatile size_t com_tx_tail;
volatile size_t com_tx_head;
volatile uint8_t com_tx_buf[32];
#define COM_TX_BUF(pos) com_tx_buf[(pos) & 0x1F]

volatile int cpu_rx_char;
static size_t cpu_rx_tail;
static size_t cpu_rx_head;
static uint8_t cpu_rx_buf[32];
#define CPU_RX_BUF(pos) cpu_rx_buf[(pos) & 0x1F]

static bool cpu_readline_active;
static const char *cpu_readline_buf;
static bool cpu_readline_needs_nl;
static size_t cpu_readline_pos;
static size_t cpu_readline_length;
static size_t cpu_str_length = 254;
static uint32_t cpu_ctrl_bits;

static int cpu_getchar_fifo(void)
{
    if (&CPU_RX_BUF(cpu_rx_head) != &CPU_RX_BUF(cpu_rx_tail))
        return CPU_RX_BUF(++cpu_rx_tail);
    return -1;
}

static void clear_com_rx_fifo()
{
    cpu_rx_char = -1;
    cpu_rx_tail = cpu_rx_head = 0;
}

static void cpu_com_rx(uint8_t ch)
{
    // discarding overflow
    if (&CPU_RX_BUF(cpu_rx_head + 1) != &CPU_RX_BUF(cpu_rx_tail))
        CPU_RX_BUF(++cpu_rx_head) = ch;
}

static void com_tx_task(void)
{
    // We sync with the UART TX FIFO so PIX STDOUT can keep pace.
    // 115_200 baud doesn't need flow control, but PIX will send
    // 16_000_000 bps if we don't throttle it.
    if (uart_get_hw(COM_UART)->fr & UART_UARTFR_TXFE_BITS)
    {
        if (&COM_TX_BUF(com_tx_head) != &COM_TX_BUF(com_tx_tail))
        {
            char ch = COM_TX_BUF(++com_tx_tail);
            uart_putc_raw(COM_UART, ch);
            if (vga_backchannel())
                pix_send_blocking(PIX_DEVICE_VGA, 0xF, 0x03, ch);
        }
    }
}

void com_init(void)
{
    gpio_set_function(COM_UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(COM_UART_RX_PIN, GPIO_FUNC_UART);
    stdio_set_driver_enabled(&com_stdio_app, true);
    uart_init(COM_UART, COM_UART_BAUD_RATE);
}

void com_run()
{
    clear_com_rx_fifo();
}

void com_stop()
{
    clear_com_rx_fifo();
    cpu_readline_active = false;
    cpu_readline_needs_nl = false;
    cpu_readline_pos = 0;
    cpu_readline_length = 0;
    cpu_str_length = 254;
    cpu_ctrl_bits = 0;
}

void com_flush(void)
{
    // Clear all buffers, software and hardware
    while (getchar_timeout_us(0) >= 0)
        com_tx_task();
    while (&COM_TX_BUF(com_tx_head) != &COM_TX_BUF(com_tx_tail))
        com_tx_task();
    while (uart_get_hw(COM_UART)->fr & UART_UARTFR_BUSY_BITS)
        tight_loop_contents();
}

void com_pre_reclock(void)
{
    com_flush();
}

void com_post_reclock(void)
{
    uart_init(COM_UART, COM_UART_BAUD_RATE);
}

void com_task(void)
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
    // if (!ria_active())
    {
        int ch = getchar_timeout_us(0);
        while (ch != PICO_ERROR_TIMEOUT)
        {
            cpu_com_rx(ch);
            ch = getchar_timeout_us(0);
        }
    }

    // Move UART FIFO into ria action loop
    if (cpu_rx_char < 0)
        cpu_rx_char = cpu_getchar_fifo();

    // Process transmit
    com_tx_task();

    // Allow UART RX FIFO to fill during RIA actions.
    // At all other times the FIFO must be emptied to detect breaks.
    // if (!ria_active())
    // {
    //     int ch;
    //     if (cpu_active() && com_callback)
    //         ch = cpu_getchar();
    //     else
    //         ch = getchar_timeout_us(0);
    //     while (ch != PICO_ERROR_TIMEOUT)
    //     {
    //         if (com_callback)
    //         {
    //             com_timer = make_timeout_time_ms(com_timeout_ms);
    //             if (com_binary_buf)
    //                 com_binary_rx(ch);
    //             else
    //                 com_line_rx(ch);
    //         }
    //         else if (cpu_active())
    //             cpu_com_rx(ch);
    //         if (ria_active()) // why?
    //             break;
    //         ch = getchar_timeout_us(0);
    //     }
    // }
}

static void com_stdio_out_chars(const char *buf, int len)
{
    while (len--)
    {
        // Wait for room in buffer before we add next char
        while (&COM_TX_BUF(com_tx_head + 1) == &COM_TX_BUF(com_tx_tail))
            com_tx_task();
        COM_TX_BUF(++com_tx_head) = *buf++;
    }
}

static int com_stdio_in_chars(char *buf, int length)
{
    // To avoid crossing the streams, we wait for a 1ms
    // pause on the UART before injecting keystrokes, then
    // keyboard buffer is emptied before returning to UART.
    static const int uart_pause_us = 1000;
    static absolute_time_t uart_timer;
    static bool in_keyboard = false;

    absolute_time_t now = get_absolute_time();
    if (in_keyboard || absolute_time_diff_us(now, uart_timer) < 0)
    {
        int i = kbd_stdio_in_chars(buf, length);
        if (i != PICO_ERROR_NO_DATA)
        {
            in_keyboard = true;
            return i;
        }
        in_keyboard = false;
    }
    if (!uart_is_readable(COM_UART))
        return PICO_ERROR_NO_DATA;
    uart_timer = delayed_by_us(now, uart_pause_us);
    int i = 0;
    while (i < length && uart_is_readable(COM_UART))
        buf[i++] = uart_getc(COM_UART);
    return i ? i : PICO_ERROR_NO_DATA;
}

static stdio_driver_t com_stdio_app = {
    .out_chars = com_stdio_out_chars,
    .in_chars = com_stdio_in_chars,
    .crlf_enabled = true,
};

int cpu_getchar(void)
{
    // Steal char from RIA register
    if (REGS(0xFFE0) & 0b01000000)
    {
        // Mixing RIA register input with read() calls isn't perfect,
        // should be considered underfined behavior, and is discouraged.
        REGS(0xFFE0) &= ~0b01000000;
        int ch = REGS(0xFFE2);
        // Replace char with null
        REGS(0xFFE2) = 0;
        return ch;
    }
    // Steal char from ria.c action loop queue
    if (cpu_rx_char >= 0)
    {
        int ch = cpu_rx_char;
        cpu_rx_char = -1;
        return ch;
    }
    // Get char from FIFO
    int ch = cpu_getchar_fifo();
    // Get char from UART
    if (ch < 0)
        ch = getchar_timeout_us(0);
    return ch;
}

static void cpu_enter(bool timeout, const char *buf, size_t length)
{
    (void)timeout;
    assert(!timeout);
    cpu_readline_active = false;
    cpu_readline_buf = buf;
    cpu_readline_pos = 0;
    cpu_readline_length = length;
    cpu_readline_needs_nl = true;
}

void cpu_stdin_request(void)
{
    if (!cpu_readline_needs_nl)
    {
        cpu_readline_active = true;
        rln_read_line(0, cpu_enter, cpu_str_length + 1, cpu_ctrl_bits);
    }
}

bool cpu_stdin_ready(void)
{
    return !cpu_readline_active;
}

size_t cpu_stdin_read(uint8_t *buf, size_t count)
{
    size_t i;
    for (i = 0; i < count && cpu_readline_pos < cpu_readline_length; i++)
        buf[i] = cpu_readline_buf[cpu_readline_pos++];
    if (i < count && cpu_readline_needs_nl)
    {
        buf[i++] = '\n';
        cpu_readline_needs_nl = false;
    }
    return i;
}

bool cpu_api_stdin_opt(void)
{
    uint8_t str_length = API_A;
    uint32_t ctrl_bits;
    if (!api_pop_uint32_end(&ctrl_bits))
        return api_return_errno(API_EINVAL);
    cpu_str_length = str_length;
    cpu_ctrl_bits = ctrl_bits;
    return api_return_ax(0);
}
