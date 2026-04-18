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
#include "net/tel.h"
#include "sys/vga.h"
#include <pico/stdlib.h>
#include <pico/stdio/driver.h>
#include <hardware/sync.h>

#if defined(DEBUG_RIA_SYS) || defined(DEBUG_RIA_SYS_COM)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

static bool com_bel_enabled = true;

/* TX — fan-out input buffers (drained by com_tx_fanout into UART + TEL).
 * com_tx_buf holds core-0 output (stdio, std_tty_write).
 * com_act_tx_buf holds core-1 act_loop output (6502 writes to 0xFFE1).
 */

volatile uint8_t com_tx_buf[COM_TX_BUF_SIZE];
volatile size_t com_tx_head;
volatile size_t com_tx_tail;

volatile uint8_t com_act_tx_buf[COM_ACT_TX_BUF_SIZE];
volatile size_t com_act_tx_head;
volatile size_t com_act_tx_tail;

/* TX — UART output buffer (internal)
 */

#define COM_UART_BUF_SIZE 32
static volatile size_t com_uart_tail;
static volatile size_t com_uart_head;
static volatile uint8_t com_uart_buf[COM_UART_BUF_SIZE];

static bool com_uart_writable(void)
{
    return (((com_uart_head + 1) % COM_UART_BUF_SIZE) != com_uart_tail);
}

static void com_uart_write(char ch)
{
    size_t next = (com_uart_head + 1) % COM_UART_BUF_SIZE;
    com_uart_buf[next] = (uint8_t)ch;
    com_uart_head = next;
}

static void com_uart_task(void)
{
    if (vga_connected())
    {
        // Use TXFE (empty) to pace VGA PIX sends
        while (com_uart_head != com_uart_tail &&
               uart_get_hw(COM_UART)->fr & UART_UARTFR_TXFE_BITS &&
               pix_ready())
        {
            size_t next = (com_uart_tail + 1) % COM_UART_BUF_SIZE;
            char ch = com_uart_buf[next];
            uart_putc_raw(COM_UART, ch);
            pix_send(PIX_DEVICE_VGA, 0xF, 0x03, ch);
            if (ch == '\a' && com_bel_enabled)
                bel_add(&bel_teletype);
            com_uart_tail = next;
        }
    }
    else
    {
        // Fill UART TX FIFO
        while (com_uart_head != com_uart_tail &&
               !(uart_get_hw(COM_UART)->fr & UART_UARTFR_TXFF_BITS))
        {
            size_t next = (com_uart_tail + 1) % COM_UART_BUF_SIZE;
            char ch = com_uart_buf[next];
            uart_putc_raw(COM_UART, ch);
            if (ch == '\a' && com_bel_enabled)
                bel_add(&bel_teletype);
            com_uart_tail = next;
        }
    }
}

// Fan out both TX rings into UART and REM buffers. One char per source
// per pass so the core-0 and core-1 streams interleave instead of one
// starving the other. __dmb() on the act_loop side pairs with the DMB
// in com_act_write() so we never read a slot whose store hasn't landed.
static void com_tx_fanout(void)
{
    while (com_uart_writable() && tel_tx_writable())
    {
        bool work = false;
        if (com_tx_head != com_tx_tail)
        {
            size_t next = (com_tx_tail + 1) % COM_TX_BUF_SIZE;
            char ch = com_tx_buf[next];
            com_uart_write(ch);
            tel_tx_write(ch);
            com_tx_tail = next;
            work = true;
            if (!com_uart_writable() || !tel_tx_writable())
                break;
        }
        if (com_act_tx_head != com_act_tx_tail)
        {
            size_t next = (com_act_tx_tail + 1) % COM_ACT_TX_BUF_SIZE;
            char ch = com_act_tx_buf[next];
            com_uart_write(ch);
            tel_tx_write(ch);
            __dmb();
            com_act_tx_tail = next;
            work = true;
        }
        if (!work)
            break;
    }
}

static void com_uart_flush(void)
{
    while (com_uart_head != com_uart_tail)
        com_uart_task();
    while (uart_get_hw(COM_UART)->fr & UART_UARTFR_BUSY_BITS)
        tight_loop_contents();
}

/* RX — merged input buffer (core-0 internal) plus the single-char
 * cross-core handoff slot com_rx_char. com_task drains com_rx_merge
 * into com_rx_buf, then refills com_rx_char from the ring when empty.
 * act_loop reads com_rx_char to serve 6502 0xFFE2 reads; the stdio
 * path steals from it when the monitor is the active consumer.
 */

#define COM_RX_BUF_SIZE 32
static size_t com_rx_head;
static size_t com_rx_tail;
static uint8_t com_rx_buf[COM_RX_BUF_SIZE];

volatile int com_rx_char = -1;

static int com_rx_buf_getchar(void)
{
    if (com_rx_head == com_rx_tail)
        return -1;
    size_t next = (com_rx_tail + 1) % COM_RX_BUF_SIZE;
    int ch = com_rx_buf[next];
    com_rx_tail = next;
    return ch;
}

// Sticky multiplex: current source holds the lock until idle for 1ms.
// Keyboard is the exception, releasing immediately when empty.
static int com_rx_merge(char *buf, int length)
{
    static const int COM_IDLE_US = 1000;
    enum source
    {
        SRC_NONE,
        SRC_KBD,
        SRC_UART,
        SRC_TEL
    };
    static enum source source = SRC_NONE;
    static absolute_time_t idle_timer;

    // Expire UART/TEL source once idle for 1ms
    if ((source == SRC_UART || source == SRC_TEL) &&
        time_reached(idle_timer))
        source = SRC_NONE;

    if (source == SRC_KBD || source == SRC_NONE)
    {
        int i = kbd_stdio_in_chars(buf, length);
        if (i != PICO_ERROR_NO_DATA)
        {
            source = SRC_KBD;
            return i;
        }
        if (source == SRC_KBD)
            source = SRC_NONE;
    }

    if (source == SRC_UART || source == SRC_NONE)
    {
        int count = 0;
        while (uart_is_readable(COM_UART) && count < length)
            buf[count++] = (uint8_t)uart_get_hw(COM_UART)->dr;
        if (count)
        {
            source = SRC_UART;
            idle_timer = make_timeout_time_us(COM_IDLE_US);
            return count;
        }
        if (source == SRC_UART)
            return PICO_ERROR_NO_DATA;
    }

    if (source == SRC_TEL || source == SRC_NONE)
    {
        int i = tel_read(buf, length);
        if (i != PICO_ERROR_NO_DATA)
        {
            source = SRC_TEL;
            idle_timer = make_timeout_time_us(COM_IDLE_US);
            return i;
        }
    }

    return PICO_ERROR_NO_DATA;
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
            com_uart_task();
            tel_pump();
        }
        com_write(*buf++);
    }
}

static void com_stdio_out_flush(void)
{
    while (com_tx_head != com_tx_tail)
    {
        com_tx_fanout();
        com_uart_task();
        tel_pump();
    }
    com_uart_flush();
    tel_flush();
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

    // Steal the cross-core handoff slot if the 6502 hasn't consumed it
    if (count < length && com_rx_char >= 0)
    {
        buf[count++] = com_rx_char;
        com_rx_char = -1;
    }

    // Drain the core-0 ring
    while (count < length)
    {
        int ch = com_rx_buf_getchar();
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
    com_uart_task();

    // TX: fan out com_tx_buf into UART and REM buffers
    com_tx_fanout();

    // RX: refill the cross-core handoff slot from the ring. __dmb()
    // publishes the slot value before act_loop on core 1 can observe it.
    if (com_rx_char < 0)
    {
        int ch = com_rx_buf_getchar();
        if (ch >= 0)
        {
            __dmb();
            com_rx_char = ch;
        }
    }

    // RX: merge sources into com_rx_buf.
    // UART doesn't detect breaks when FIFO is full
    // so we keep it drained and discard overruns like the UART would.
    char ch;
    while (com_rx_merge(&ch, 1) == 1)
    {
        size_t next = (com_rx_head + 1) % COM_RX_BUF_SIZE;
        if (next != com_rx_tail)
        {
            com_rx_buf[next] = (uint8_t)ch;
            com_rx_head = next;
        }
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
