/*
 * Copyright (c) 2026 Rumbledethumps
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
#include "net/cyw.h"
#include "net/wfi.h"
#include "sys/cfg.h"
#include "str/str.h"
#include <stdlib.h>
#include <string.h>
#include <pico/stdlib.h>
#include <pico/stdio/driver.h>
#include <hardware/sync.h>

#if defined(DEBUG_RIA_SYS) || defined(DEBUG_RIA_SYS_COM)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

/* Shared state — TX tee sources, UART ring, merged RX ring, BEL flag.
 * com_tx_buf holds core-0 output (stdio, std_tty_write); com_act_tx_buf
 * holds core-1 act_loop output (6502 writes to 0xFFE1). Both are drained
 * by com_tx_fanout into UART + telnet (when W).
 */

volatile uint8_t com_tx_buf[COM_TX_BUF_SIZE];
volatile size_t com_tx_head;
volatile size_t com_tx_tail;

volatile uint8_t com_act_tx_buf[COM_ACT_TX_BUF_SIZE];
volatile size_t com_act_tx_head;
volatile size_t com_act_tx_tail;

#define COM_UART_BUF_SIZE 32
static volatile size_t com_uart_tail;
static volatile size_t com_uart_head;
static volatile uint8_t com_uart_buf[COM_UART_BUF_SIZE];

// com_task drains multiplexed sources into com_rx_buf, then refills
// com_rx_char from the ring when empty. act_loop on core 1 reads
// com_rx_char to serve 6502 0xFFE2 reads; the stdio path steals from it
// when the monitor is the active consumer.
#define COM_RX_BUF_SIZE 32
static size_t com_rx_head;
static size_t com_rx_tail;
static uint8_t com_rx_buf[COM_RX_BUF_SIZE];
volatile int com_rx_char = -1;

static bool com_bel_enabled = true;

#ifndef RP6502_RIA_W

static bool com_tel_tx_writable(void) { return true; }
static void com_tel_tx_write(char) {}
static int com_tel_read(char *, int) { return PICO_ERROR_NO_DATA; }
static void com_tel_pump(void) {}
static void com_tel_flush(void) {}
static void com_tel_task(void) {}

#else

#define COM_TEL_KEY_SIZE 33
static uint16_t com_tel_port = 23;
static char com_tel_key[COM_TEL_KEY_SIZE];

typedef enum
{
    com_tel_state_idle,
    com_tel_state_listening,
    com_tel_state_auth,
    com_tel_state_connected,
} com_tel_state_t;
static com_tel_state_t com_tel_state = com_tel_state_idle;
static uint16_t com_tel_active_port;

static char com_tel_auth_buf[COM_TEL_KEY_SIZE];
static uint8_t com_tel_auth_len;

#define COM_TEL_TX_BUF_SIZE 32
static char com_tel_tx_buf[COM_TEL_TX_BUF_SIZE];
static volatile size_t com_tel_tx_head;
static volatile size_t com_tel_tx_tail;

#define COM_TEL_RX_BUF_SIZE 32
static char com_tel_rx_buf[COM_TEL_RX_BUF_SIZE];
static size_t com_tel_rx_head;
static size_t com_tel_rx_tail;

static void com_tel_rings_clear(void)
{
    com_tel_tx_head = com_tel_tx_tail = 0;
    com_tel_rx_head = com_tel_rx_tail = 0;
}

static bool com_tel_tx_writable(void)
{
    return ((com_tel_tx_head + 1) % COM_TEL_TX_BUF_SIZE) != com_tel_tx_tail;
}

static void com_tel_tx_write(char ch)
{
    com_tel_tx_head = (com_tel_tx_head + 1) % COM_TEL_TX_BUF_SIZE;
    com_tel_tx_buf[com_tel_tx_head] = ch;
}

static int com_tel_read(char *buf, int length)
{
    int count = 0;
    while (count < length && com_tel_rx_head != com_tel_rx_tail)
    {
        com_tel_rx_tail = (com_tel_rx_tail + 1) % COM_TEL_RX_BUF_SIZE;
        buf[count++] = com_tel_rx_buf[com_tel_rx_tail];
    }
    return count ? count : PICO_ERROR_NO_DATA;
}

static void com_tel_drain_tx(void)
{
    if (com_tel_state != com_tel_state_connected)
    {
        // Discard — nobody to send to
        com_tel_tx_tail = com_tel_tx_head;
        return;
    }
    if (com_tel_tx_tail == com_tel_tx_head)
        return;
    size_t start = (com_tel_tx_tail + 1) % COM_TEL_TX_BUF_SIZE;
    size_t len;
    if (com_tel_tx_head >= start)
        len = com_tel_tx_head - start + 1;
    else
        len = COM_TEL_TX_BUF_SIZE - start;
    uint16_t sent = tel_tx(SYS_TEL_DESC, &com_tel_tx_buf[start], len);
    com_tel_tx_tail = (com_tel_tx_tail + sent) % COM_TEL_TX_BUF_SIZE;
}

static void com_tel_pump(void)
{
    com_tel_drain_tx();
    if (com_tel_tx_head != com_tel_tx_tail)
        cyw_task();
}

static void com_tel_flush(void)
{
    while (com_tel_state == com_tel_state_connected &&
           com_tel_tx_head != com_tel_tx_tail)
        com_tel_pump();
}

static void com_tel_handle_auth(uint8_t ch)
{
    if (ch == '\b' || ch == 127)
    {
        if (com_tel_auth_len > 0)
        {
            com_tel_auth_len--;
            tel_tx(SYS_TEL_DESC, "\b \b", 3);
        }
    }
    else if (ch == '\r' || ch == '\n')
    {
        com_tel_auth_buf[com_tel_auth_len] = 0;
        if (strcmp(com_tel_auth_buf, com_tel_key) == 0)
        {
            tel_tx(SYS_TEL_DESC, STR_TEL_CONNECTED, STR_TEL_CONNECTED_LEN);
            com_tel_state = com_tel_state_connected;
            vga_set_tel_console_active(true);
            DBG("NET TEL console authenticated\n");
        }
        else
        {
            tel_tx(SYS_TEL_DESC, STR_TEL_ACCESS_DENIED, STR_TEL_ACCESS_DENIED_LEN);
            DBG("NET TEL console auth failed\n");
            com_tel_state = com_tel_state_listening;
            tel_close(SYS_TEL_DESC);
        }
    }
    else if (ch >= 32 && ch < 127 && com_tel_auth_len < COM_TEL_KEY_SIZE - 1)
    {
        com_tel_auth_buf[com_tel_auth_len++] = ch;
        tel_tx(SYS_TEL_DESC, "*", 1);
    }
}

static void com_tel_drain_rx(void)
{
    // Limit read to ring buffer free space so we never drop data.
    // Decoded bytes <= raw bytes, so this guarantees room.
    uint16_t limit = COM_TEL_RX_BUF_SIZE;
    if (com_tel_state == com_tel_state_connected)
    {
        size_t used = (com_tel_rx_head - com_tel_rx_tail + COM_TEL_RX_BUF_SIZE) % COM_TEL_RX_BUF_SIZE;
        limit = COM_TEL_RX_BUF_SIZE - 1 - used;
        if (limit == 0)
            return;
    }

    char decoded[COM_TEL_RX_BUF_SIZE];
    uint16_t decoded_len = tel_rx(SYS_TEL_DESC, decoded, limit);

    for (uint16_t i = 0; i < decoded_len; i++)
    {
        uint8_t ch = (uint8_t)decoded[i];
        if (com_tel_state == com_tel_state_auth)
        {
            com_tel_handle_auth(ch);
            if (com_tel_state != com_tel_state_auth)
                return;
        }
        else if (com_tel_state == com_tel_state_connected)
        {
            com_tel_rx_head = (com_tel_rx_head + 1) % COM_TEL_RX_BUF_SIZE;
            com_tel_rx_buf[com_tel_rx_head] = ch;
        }
    }
}

static bool com_tel_should_listen(void)
{
    return com_tel_port > 0 && com_tel_key[0] != 0 && wfi_ready();
}

static void com_tel_shutdown(void)
{
    if (com_tel_state == com_tel_state_auth || com_tel_state == com_tel_state_connected)
        tel_close(SYS_TEL_DESC);
    if (com_tel_state >= com_tel_state_listening)
    {
        tel_listen_close(com_tel_active_port);
        com_tel_active_port = 0;
    }
    com_tel_state = com_tel_state_idle;
    com_tel_rings_clear();
}

static void com_tel_on_disconnect(int desc)
{
    if (com_tel_state == com_tel_state_auth || com_tel_state == com_tel_state_connected)
    {
        DBG("NET TEL console disconnected\n");
        if (com_tel_state == com_tel_state_connected)
            vga_set_tel_console_active(false);
        com_tel_state = com_tel_state_listening;
        com_tel_rings_clear();
    }
    tel_close(desc);
}

static bool com_tel_on_accept(uint16_t port)
{
    if (com_tel_state != com_tel_state_listening)
        return false;

    if (!tel_accept_server(SYS_TEL_DESC, port, com_tel_on_disconnect))
        return false;

    tel_tx(SYS_TEL_DESC, STR_TEL_PASSKEY, STR_TEL_PASSKEY_LEN);

    com_tel_auth_len = 0;
    com_tel_rings_clear();
    com_tel_state = com_tel_state_auth;
    DBG("NET TEL console accepted, awaiting auth\n");
    return true;
}

void com_tel_load_port(const char *str)
{
    com_tel_port = atoi(str);
}

void com_tel_load_key(const char *str)
{
    size_t n = strlen(str);
    if (n < COM_TEL_KEY_SIZE)
    {
        memcpy(com_tel_key, str, n);
        com_tel_key[n] = 0;
    }
}

bool com_tel_set_port(uint32_t port)
{
    if (port > 65535)
        return false;
    if (com_tel_port != (uint16_t)port)
    {
        com_tel_port = port;
        if (port == 0)
            com_tel_shutdown();
        cfg_save();
    }
    return true;
}

bool com_tel_set_key(const char *key)
{
    size_t n = strlen(key);
    if (n >= COM_TEL_KEY_SIZE)
        return false;
    if (strcmp(com_tel_key, key))
    {
        memcpy(com_tel_key, key, n);
        com_tel_key[n] = 0;
        if (com_tel_key[0] == 0)
            com_tel_shutdown();
        cfg_save();
    }
    return true;
}

uint16_t com_tel_get_port(void)
{
    return com_tel_port;
}

const char *com_tel_get_key(void)
{
    return com_tel_key;
}

static void com_tel_task(void)
{
    com_tel_drain_tx();
    switch (com_tel_state)
    {
    case com_tel_state_idle:
        if (com_tel_should_listen())
        {
            if (tel_listen(com_tel_port, com_tel_on_accept))
            {
                com_tel_active_port = com_tel_port;
                com_tel_state = com_tel_state_listening;
                DBG("NET TEL console listening on port %u\n", com_tel_port);
            }
        }
        break;
    case com_tel_state_listening:
        if (!com_tel_should_listen() || com_tel_active_port != com_tel_port)
            com_tel_shutdown();
        break;
    case com_tel_state_auth:
    case com_tel_state_connected:
        if (!com_tel_should_listen() || com_tel_active_port != com_tel_port)
            com_tel_shutdown();
        else
            com_tel_drain_rx();
        break;
    }
}

#endif

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

static void com_uart_flush(void)
{
    while (com_uart_head != com_uart_tail)
        com_uart_task();
    while (uart_get_hw(COM_UART)->fr & UART_UARTFR_BUSY_BITS)
        tight_loop_contents();
}

// One char per source per pass so the core-0 and core-1 streams interleave
// instead of one starving the other. The consumer-side __dmb() below pairs
// with the producer DMB in com_act_write(): it finishes reading the slot
// before publishing the tail advance so the producer can't overwrite an
// in-flight read.
static void com_tx_fanout(void)
{
    while (com_uart_writable() && com_tel_tx_writable())
    {
        bool work = false;
        if (com_tx_head != com_tx_tail)
        {
            size_t next = (com_tx_tail + 1) % COM_TX_BUF_SIZE;
            char ch = com_tx_buf[next];
            com_uart_write(ch);
            com_tel_tx_write(ch);
            com_tx_tail = next;
            work = true;
            if (!com_uart_writable() || !com_tel_tx_writable())
                break;
        }
        if (com_act_tx_head != com_act_tx_tail)
        {
            size_t next = (com_act_tx_tail + 1) % COM_ACT_TX_BUF_SIZE;
            char ch = com_act_tx_buf[next];
            com_uart_write(ch);
            com_tel_tx_write(ch);
            __dmb();
            com_act_tx_tail = next;
            work = true;
        }
        if (!work)
            break;
    }
}

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
    enum com_rx_src
    {
        SRC_NONE,
        SRC_KBD,
        SRC_UART,
        SRC_TEL
    };
    static enum com_rx_src source = SRC_NONE;
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
        int i = com_tel_read(buf, length);
        if (i != PICO_ERROR_NO_DATA)
        {
            source = SRC_TEL;
            idle_timer = make_timeout_time_us(COM_IDLE_US);
            return i;
        }
    }

    return PICO_ERROR_NO_DATA;
}

static void com_stdio_out_chars(const char *buf, int len)
{
    while (len--)
    {
        while (!com_writable())
        {
            com_tx_fanout();
            com_uart_task();
            com_tel_pump();
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
        com_tel_pump();
    }
    com_uart_flush();
    com_tel_flush();
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

void com_init(void)
{
    gpio_pull_up(COM_UART_TX_PIN);
    gpio_pull_up(COM_UART_RX_PIN);
    gpio_set_function(COM_UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(COM_UART_RX_PIN, GPIO_FUNC_UART);
    stdio_set_driver_enabled(&com_stdio_driver, true);
    uart_init(COM_UART, COM_UART_BAUD_RATE);
    // Wait for the UART to settle after VGA startup then purge everything.
    busy_wait_ms(25);
    while (stdio_getchar_timeout_us(0) != PICO_ERROR_TIMEOUT)
        tight_loop_contents();
    hw_clear_bits(&uart_get_hw(COM_UART)->rsr, UART_UARTRSR_BITS);
}

void com_stop(void)
{
    com_rx_char = -1;
    com_rx_head = com_rx_tail = 0;
}

void com_task(void)
{
    // TX: drain UART buffer to hardware
    com_uart_task();

    // TX: fan out com_tx_buf into UART and TEL buffers
    com_tx_fanout();

    // RX: refill the cross-core handoff slot from the ring. com_rx_char is
    // a single volatile int so the publish is atomic; the __dmb() is a
    // defensive fence.
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

    com_tel_task();
}

bool com_get_bel(void)
{
    return com_bel_enabled;
}

void com_set_bel(bool value)
{
    com_bel_enabled = value;
}
