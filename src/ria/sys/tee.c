/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sys/tee.h"
#include "sys/com.h"
#include "sys/mem.h"
#include "sys/rem.h"
#include "hid/kbd.h"
#include <pico/stdlib.h>
#include <pico/stdio/driver.h>

#if defined(DEBUG_RIA_SYS) || defined(DEBUG_RIA_SYS_TEE)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

/* TX — tee input buffer
 */

volatile uint8_t tee_buf[TEE_BUF_SIZE];
volatile size_t tee_head;
volatile size_t tee_tail;

/* RX — merged input buffer
 */

#define TEE_RX_BUF_SIZE 32
static uint8_t tee_rx_buf[TEE_RX_BUF_SIZE];
static size_t tee_rx_head;
static size_t tee_rx_tail;
volatile int tee_rx_char;

static int tee_rx_getchar(void)
{
    if (tee_rx_head != tee_rx_tail)
    {
        tee_rx_tail = (tee_rx_tail + 1) % TEE_RX_BUF_SIZE;
        return tee_rx_buf[tee_rx_tail];
    }
    return -1;
}

// Multiplex input sources with 1ms UART pause priority
static int tee_rx_merge(char *buf, int length)
{
    static bool in_keyboard = false;
    static const int TEE_UART_PAUSE_US = 1000;
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

    int count = com_rx(buf, length);
    if (count != PICO_ERROR_NO_DATA)
        uart_timer = make_timeout_time_us(TEE_UART_PAUSE_US);

    return count;
}

/* stdio driver
 */

static void tee_stdio_out_chars(const char *buf, int len)
{
    while (len--)
    {
        while (!tee_writable())
        {
            tee_task();
            com_pump();
            rem_pump();
        }
        char ch = *buf++;
        tee_head = (tee_head + 1) % TEE_BUF_SIZE;
        tee_buf[tee_head] = ch;
    }
}

static void tee_stdio_out_flush(void)
{
    while (tee_head != tee_tail)
    {
        tee_task();
        com_pump();
        rem_pump();
    }
    com_flush();
    rem_flush();
}

static int tee_stdio_in_chars(char *buf, int length)
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
    if (count < length && tee_rx_char >= 0)
    {
        int ch = tee_rx_char;
        tee_rx_char = -1;
        buf[count++] = ch;
    }

    // Take from the circular buffer
    while (count < length)
    {
        int ch = tee_rx_getchar();
        if (ch < 0)
            break;
        buf[count++] = ch;
    }

    // Pick up new chars
    int x = tee_rx_merge(&buf[count], length - count);
    if (x != PICO_ERROR_NO_DATA)
        count += x;

    return count ? count : PICO_ERROR_NO_DATA;
}

static stdio_driver_t tee_stdio_driver = {
    .out_chars = tee_stdio_out_chars,
    .out_flush = tee_stdio_out_flush,
    .in_chars = tee_stdio_in_chars,
    .crlf_enabled = true,
};

/* Main events
 */

void tee_init(void)
{
    stdio_set_driver_enabled(&tee_stdio_driver, true);
    // Wait for the UART to settle then purge everything.
    // If we leave garbage then there is a chance for
    // no startup message because break clears it or
    // VGA detection will fail to detect.
    busy_wait_ms(5); // 2 fails, 3 works, 5 for safety
    while (stdio_getchar_timeout_us(0) != PICO_ERROR_TIMEOUT)
        tight_loop_contents();
    hw_clear_bits(&uart_get_hw(COM_UART)->rsr, UART_UARTRSR_BITS);
}

void tee_task(void)
{
    // TX: fan out tee_buf into com and rem
    while (tee_head != tee_tail &&
           com_tx_writable() && rem_tx_writable())
    {
        tee_tail = (tee_tail + 1) % TEE_BUF_SIZE;
        char ch = tee_buf[tee_tail];
        com_tx_write(ch);
        rem_tx_write(ch);
    }

    // RX: fill tee_rx_char from tee_rx_buf
    if (tee_rx_char < 0)
        tee_rx_char = tee_rx_getchar();

    // RX: merge sources into tee_rx_buf.
    // UART doesn't detect breaks when FIFO is full
    // so we keep it drained and discard overruns like the UART would.
    char ch;
    while (tee_rx_merge(&ch, 1) == 1)
        if (((tee_rx_head + 1) % TEE_RX_BUF_SIZE) != tee_rx_tail)
        {
            tee_rx_head = (tee_rx_head + 1) % TEE_RX_BUF_SIZE;
            tee_rx_buf[tee_rx_head] = ch;
        }
}

void tee_run(void)
{
    tee_rx_char = -1;
    tee_rx_tail = tee_rx_head = 0;
}

void tee_stop(void)
{
    tee_rx_char = -1;
    tee_rx_tail = tee_rx_head = 0;
}
