/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "api/api.h"
#include "hid/kbd.h"
#include "sys/com.h"
#include "sys/pix.h"
#include "sys/rln.h"
#include "sys/vga.h"
#include <pico/stdlib.h>
#include <pico/stdio/driver.h>

#if defined(DEBUG_RIA_SYS) || defined(DEBUG_RIA_SYS_COM)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define COM_BUF_SIZE 256
#define COM_CSI_PARAM_MAX_LEN 16

static stdio_driver_t com_stdio_driver;

volatile size_t com_tx_tail;
volatile size_t com_tx_head;
volatile uint8_t com_tx_buf[COM_TX_BUF_SIZE];

#define COM_RX_BUF_SIZE 32
static size_t com_rx_tail;
static size_t com_rx_head;
static uint8_t com_rx_buf[COM_RX_BUF_SIZE];
volatile int com_rx_char;

static int com_rx_buf_getchar(void)
{
    if (com_rx_head != com_rx_tail)
    {
        com_rx_tail = (com_rx_tail + 1) % COM_RX_BUF_SIZE;
        return com_rx_buf[com_rx_tail];
    }
    return -1;
}

static void com_clear_all_rx()
{
    com_rx_char = -1;
    com_rx_tail = com_rx_head = 0;
}

static void com_tx_task(void)
{
    while (com_tx_head != com_tx_tail &&
           uart_get_hw(COM_UART)->fr & UART_UARTFR_TXFE_BITS)
    {
        if (vga_connected())
        {
            if (!pix_ready())
                break;
            com_tx_tail = (com_tx_tail + 1) % COM_TX_BUF_SIZE;
            char ch = com_tx_buf[com_tx_tail];
            pix_send(PIX_DEVICE_VGA, 0xF, 0x03, ch);
            // Keep pace with UART TX
            uart_putc_raw(COM_UART, ch);
        }
        else
        {
            com_tx_tail = (com_tx_tail + 1) % COM_TX_BUF_SIZE;
            char ch = com_tx_buf[com_tx_tail];
            uart_putc_raw(COM_UART, ch);
        }
    }
}

static int com_rx_task(char *buf, int length)
{
    // To avoid crossing the streams, we wait for a 1ms
    // pause on the UART before injecting keystrokes, then
    // keyboard buffer is emptied before returning to UART.
    static bool in_keyboard = false;
    static const int COM_STDIO_UART_PAUSE_US = 1000;
    static absolute_time_t com_stdio_uart_timer;

    if (in_keyboard || absolute_time_diff_us(get_absolute_time(), com_stdio_uart_timer) < 0)
    {
        int i = kbd_stdio_in_chars(buf, length);
        if (i != PICO_ERROR_NO_DATA)
        {
            in_keyboard = true;
            return i;
        }
        in_keyboard = false;
    }

    // Get char from UART
    int count = 0;
    bool readable = uart_is_readable(COM_UART);
    if (readable)
        com_stdio_uart_timer = make_timeout_time_us(COM_STDIO_UART_PAUSE_US);
    while (readable && count < length)
    {
        buf[count++] = (uint8_t)uart_get_hw(COM_UART)->dr;
        readable = uart_is_readable(COM_UART);
    }

    return count ? count : PICO_ERROR_NO_DATA;
}

void com_init(void)
{
    gpio_set_function(COM_UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(COM_UART_RX_PIN, GPIO_FUNC_UART);
    stdio_set_driver_enabled(&com_stdio_driver, true);
    uart_init(COM_UART, COM_UART_BAUD_RATE);
    com_clear_all_rx();
    // Wait for the UART to settle then purge everything.
    // If we leave garbage then there is a chance for
    // no startup message because break clears it or
    // VGA detection will fail to detect.
    busy_wait_ms(5); // 2 fails, 3 works, 5 for safety
    while (stdio_getchar_timeout_us(0) != PICO_ERROR_TIMEOUT)
        tight_loop_contents();
    hw_clear_bits(&uart_get_hw(COM_UART)->rsr, UART_UARTRSR_BITS);
}

void com_run()
{
    com_clear_all_rx();
}

void com_stop()
{
    com_clear_all_rx();
}

void com_task(void)
{
    // Process transmit.
    com_tx_task();

    // Move char into ria action loop.
    if (com_rx_char < 0)
        com_rx_char = com_rx_buf_getchar();

    // Process receive. UART doesn't detect breaks when FIFO is full
    // so we keep it drained and discard overruns like the UART would.
    char ch;
    while (com_rx_task(&ch, 1) == 1)
        if (((com_rx_head + 1) % COM_RX_BUF_SIZE) != com_rx_tail)
        {
            com_rx_head = (com_rx_head + 1) % COM_RX_BUF_SIZE;
            com_rx_buf[com_rx_head] = ch;
        }

    // Detect UART breaks.
    static uint32_t break_detect = 0;
    uint32_t current_break = uart_get_hw(COM_UART)->rsr & UART_UARTRSR_BE_BITS;
    if (current_break)
        hw_clear_bits(&uart_get_hw(COM_UART)->rsr, UART_UARTRSR_BITS);
    else if (break_detect)
        main_break();
    break_detect = current_break;
}

static void com_stdio_out_chars(const char *buf, int len)
{
    while (len--)
    {
        // Wait for room in buffer before we add next char
        while ((com_tx_head + 1) % COM_TX_BUF_SIZE == com_tx_tail)
            com_tx_task();
        com_tx_head = (com_tx_head + 1) % COM_TX_BUF_SIZE;
        com_tx_buf[com_tx_head] = *buf++;
    }
}

static void com_stdio_out_flush(void)
{
    while (com_tx_head != com_tx_tail)
        com_tx_task();
    while (uart_get_hw(COM_UART)->fr & UART_UARTFR_BUSY_BITS)
        tight_loop_contents();
}

static int com_stdio_in_chars(char *buf, int length)
{
    int count = 0;

    // Take char from RIA register
    if (count < length && REGS(0xFFE0) & 0b01000000)
    {
        // Mixing RIA register input with read() calls isn't perfect,
        // should be considered underfined behavior, and is discouraged.
        REGS(0xFFE0) &= ~0b01000000;
        int ch = REGS(0xFFE2);
        // Replace char with ASCII NUL
        REGS(0xFFE2) = 0;
        buf[count++] = ch;
    }

    // Take char from ria.c action loop queue
    if (count < length && com_rx_char >= 0)
    {
        int ch = com_rx_char;
        com_rx_char = -1;
        buf[count++] = ch;
    }

    // Take from the circular buffer
    while (count < length)
    {
        int ch = com_rx_buf_getchar();
        if (ch < 0)
            break;
        buf[count++] = ch;
    }

    // Pick up new chars from uart or keyboard
    int x = com_rx_task(&buf[count], length - count);
    if (x != PICO_ERROR_NO_DATA)
        count += x;

    return count ? count : PICO_ERROR_NO_DATA;
}

static stdio_driver_t com_stdio_driver = {
    .out_chars = com_stdio_out_chars,
    .out_flush = com_stdio_out_flush,
    .in_chars = com_stdio_in_chars,
    .crlf_enabled = true,
};
