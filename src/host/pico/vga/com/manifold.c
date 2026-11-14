/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "vga/com/manifold.h"
#include "core/sys/ring.h"
#include "vga/usb/cdc.h"
#include <tusb.h>
#include <pico/stdlib.h>
#include <hardware/clocks.h>
#include <pico/stdio/driver.h>

static RING(char, MANIFOLD_IN_BUF_SIZE) manifold_in;

static RING(char, MANIFOLD_OUT_BUF_SIZE) manifold_out;

size_t manifold_in_free(void)
{
    return RING_AVAIL(&manifold_in);
}

bool manifold_in_empty(void)
{
    return !RING_LEN(&manifold_in);
}

void manifold_in_write(char ch)
{
    RING_PUT(&manifold_in, ch);
}

/* The RIA answers the program's queries; its terminal follows this one. */
void manifold_reply(const char *s, size_t n)
{
    (void)s;
    (void)n;
}

bool manifold_out_empty(void)
{
    return !RING_LEN(&manifold_out);
}

bool manifold_out_full(void)
{
    return !RING_AVAIL(&manifold_out);
}

char manifold_out_peek(void)
{
    return *RING_AT(&manifold_out, 0);
}

char manifold_out_read(void)
{
    char ch = 0;
    RING_GET(&manifold_out, &ch);
    return ch;
}

static void manifold_out_chars(const char *buf, int length)
{
    for (;;)
    {
        const uint32_t n = RING_IN(&manifold_out, buf, (uint32_t)length);
        buf += n;
        length -= (int)n;
        if (!length)
            break;
        cdc_task();
        tud_task();
    }
}

void manifold_init(void)
{
    /* The UART runs from the USB PLL, so changing the system clock for a new
     * display never touches it. Resetting it after a reclock instead, with
     * the RIA part way through a burst of text, left the receiver out of step
     * with the bytes until the line next went idle. set_sys_clock_khz puts
     * clk_peri here too, unless PICO_CLOCK_ADJUST_PERI_CLOCK_WITH_SYS_CLOCK
     * is set. */
    clock_configure_undivided(clk_peri, 0,
                              CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                              USB_CLK_HZ);
    gpio_pull_up(MANIFOLD_UART_TX_PIN);
    gpio_pull_up(MANIFOLD_UART_RX_PIN);
    gpio_set_function(MANIFOLD_UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(MANIFOLD_UART_RX_PIN, GPIO_FUNC_UART);
    uart_init(MANIFOLD_UART_INTERFACE, MANIFOLD_UART_BAUDRATE);

    static stdio_driver_t stdio_driver = {
        .out_chars = manifold_out_chars,
        .crlf_enabled = true,
    };
    stdio_set_driver_enabled(&stdio_driver, true);
}

static stdio_driver_t manifold_term_driver = {.crlf_enabled = true};

void manifold_set_term_out(void (*out_chars)(const char *buf, int len))
{
    manifold_term_driver.out_chars = out_chars;
    stdio_set_driver_enabled(&manifold_term_driver, out_chars != NULL);
}

void manifold_term_putc(char ch)
{
    manifold_term_driver.out_chars(&ch, 1);
}

void manifold_set_uart_break(bool en)
{
    uart_set_break(MANIFOLD_UART_INTERFACE, en);
}

void manifold_task(void)
{
    // IN is sunk here to UART
    char ch;
    while (uart_is_writable(MANIFOLD_UART_INTERFACE) && RING_GET(&manifold_in, &ch))
        uart_get_hw(MANIFOLD_UART_INTERFACE)->dr = ch;

    // OUT is sourced here from STD UART
    while (uart_is_readable(MANIFOLD_UART_INTERFACE))
        putchar_raw(uart_getc(MANIFOLD_UART_INTERFACE));
}
