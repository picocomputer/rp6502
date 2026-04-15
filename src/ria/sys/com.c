/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "aud/bel.h"
#include "hid/kbd.h"
#include "sys/com.h"
#include "sys/mem.h"
#include "sys/pix.h"
#include "sys/rem.h"
#include "sys/vga.h"
#include <pico/stdlib.h>
#include <pico/stdio/driver.h>

#if defined(DEBUG_RIA_SYS) || defined(DEBUG_RIA_SYS_COM)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

static bool com_bel_enabled = true;

/* TX — fan-out input buffer (public, written by core1 act_loop)
 */

volatile uint8_t com_buf[COM_BUF_SIZE];
volatile size_t com_head;
volatile size_t com_tail;

/* TX — UART output buffer (internal)
 */

#define COM_TX_BUF_SIZE 32
static volatile size_t com_tx_tail;
static volatile size_t com_tx_head;
static volatile uint8_t com_tx_buf[COM_TX_BUF_SIZE];

static bool com_tx_writable(void)
{
    return (((com_tx_head + 1) % COM_TX_BUF_SIZE) != com_tx_tail);
}

static void com_tx_write(char ch)
{
    com_tx_head = (com_tx_head + 1) % COM_TX_BUF_SIZE;
    com_tx_buf[com_tx_head] = ch;
}

static void com_tx_task(void)
{
    if (vga_connected())
    {
        // Use TXFE (empty) to pace VGA PIX sends
        while (com_tx_head != com_tx_tail &&
               uart_get_hw(COM_UART)->fr & UART_UARTFR_TXFE_BITS &&
               pix_ready())
        {
            com_tx_tail = (com_tx_tail + 1) % COM_TX_BUF_SIZE;
            char ch = com_tx_buf[com_tx_tail];
            uart_putc_raw(COM_UART, ch);
            pix_send(PIX_DEVICE_VGA, 0xF, 0x03, ch);
            if (ch == '\a' && com_bel_enabled)
                bel_add(&bel_teletype);
        }
    }
    else
    {
        // Fill UART TX FIFO
        while (com_tx_head != com_tx_tail &&
               !(uart_get_hw(COM_UART)->fr & UART_UARTFR_TXFF_BITS))
        {
            com_tx_tail = (com_tx_tail + 1) % COM_TX_BUF_SIZE;
            char ch = com_tx_buf[com_tx_tail];
            uart_putc_raw(COM_UART, ch);
            if (ch == '\a' && com_bel_enabled)
                bel_add(&bel_teletype);
        }
    }
}

// Fan out com_buf into UART and REM buffers
static void com_tx_fanout(void)
{
    while (com_head != com_tail &&
           com_tx_writable() && rem_tx_writable())
    {
        com_tail = (com_tail + 1) % COM_BUF_SIZE;
        char ch = com_buf[com_tail];
        com_tx_write(ch);
        rem_tx_write(ch);
    }
}

static void com_tx_flush(void)
{
    while (com_tx_head != com_tx_tail)
        com_tx_task();
    while (uart_get_hw(COM_UART)->fr & UART_UARTFR_BUSY_BITS)
        tight_loop_contents();
}

/* RX — merged input buffer (internal)
 */

#define COM_RX_BUF_SIZE 32
static uint8_t com_rx_buf[COM_RX_BUF_SIZE];
static size_t com_rx_head;
static size_t com_rx_tail;
volatile int com_rx_char;

static int com_rx_getchar(void)
{
    if (com_rx_head != com_rx_tail)
    {
        com_rx_tail = (com_rx_tail + 1) % COM_RX_BUF_SIZE;
        return com_rx_buf[com_rx_tail];
    }
    return -1;
}

// Multiplex input sources with 1ms UART pause priority
static int com_rx_merge(char *buf, int length)
{
    static bool in_keyboard = false;
    static const int COM_UART_PAUSE_US = 1000;
    static absolute_time_t uart_timer;

    if (in_keyboard || absolute_time_diff_us(get_absolute_time(), uart_timer) < 0)
    {
        int i = kbd_stdio_in_chars(buf, length);
        if (i != PICO_ERROR_NO_DATA)
        {
            in_keyboard = true;
            return i;
        }
        i = rem_rx(buf, length);
        if (i != PICO_ERROR_NO_DATA)
        {
            in_keyboard = true;
            return i;
        }
        in_keyboard = false;
    }

    // Read UART
    int count = 0;
    while (uart_is_readable(COM_UART) && count < length)
        buf[count++] = (uint8_t)uart_get_hw(COM_UART)->dr;
    if (count)
        uart_timer = make_timeout_time_us(COM_UART_PAUSE_US);

    return count ? count : PICO_ERROR_NO_DATA;
}

/* stdio driver
 */

static void com_stdio_out_chars(const char *buf, int len)
{
    while (len--)
    {
        while (!com_writable())
        {
            com_tx_fanout();
            com_tx_task();
            rem_pump();
        }
        char ch = *buf++;
        com_head = (com_head + 1) % COM_BUF_SIZE;
        com_buf[com_head] = ch;
    }
}

static void com_stdio_out_flush(void)
{
    while (com_head != com_tail)
    {
        com_tx_fanout();
        com_tx_task();
        rem_pump();
    }
    com_tx_flush();
    rem_flush();
}

static int com_stdio_in_chars(char *buf, int length)
{
    int count = 0;

    // Take char from RIA register
    if (count < length && REGS(0xFFE0) & 0b01000000)
    {
        buf[count++] = REGS(0xFFE2);
        REGS(0xFFE2) = 0;
        REGS(0xFFE0) = 0;
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
        int ch = com_rx_getchar();
        if (ch < 0)
            break;
        buf[count++] = ch;
    }

    // Pick up new chars
    int x = com_rx_merge(&buf[count], length - count);
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

/* Main events
 */

void com_init(void)
{
    gpio_set_function(COM_UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(COM_UART_RX_PIN, GPIO_FUNC_UART);
    stdio_set_driver_enabled(&com_stdio_driver, true);
    uart_init(COM_UART, COM_UART_BAUD_RATE);
    com_rx_char = -1;
    // Wait for the UART to settle then purge everything.
    // If we leave garbage then there is a chance for
    // no startup message because break clears it or
    // VGA detection will fail to detect.
    busy_wait_ms(5); // 2 fails, 3 works, 5 for safety
    while (stdio_getchar_timeout_us(0) != PICO_ERROR_TIMEOUT)
        tight_loop_contents();
    hw_clear_bits(&uart_get_hw(COM_UART)->rsr, UART_UARTRSR_BITS);
}

void com_task(void)
{
    // TX: drain UART buffer to hardware
    com_tx_task();

    // TX: fan out com_buf into UART and REM buffers
    com_tx_fanout();

    // RX: fill com_rx_char from com_rx_buf
    if (com_rx_char < 0)
        com_rx_char = com_rx_getchar();

    // RX: merge sources into com_rx_buf.
    // UART doesn't detect breaks when FIFO is full
    // so we keep it drained and discard overruns like the UART would.
    char ch;
    while (com_rx_merge(&ch, 1) == 1)
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

bool com_get_bel(void)
{
    return com_bel_enabled;
}

void com_set_bel(bool value)
{
    com_bel_enabled = value;
}
