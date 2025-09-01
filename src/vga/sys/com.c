/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sys/com.h"
#include "usb/cdc.h"
#include <tusb.h>
#include <pico/stdlib.h>
#include <pico/stdio/driver.h>

size_t com_in_head;
size_t com_in_tail;
char com_in_buf[COM_IN_BUF_SIZE];

size_t com_out_head;
size_t com_out_tail;
char com_out_buf[COM_OUT_BUF_SIZE];

size_t com_in_free(void)
{
    return ((com_in_tail + 0) - (com_in_head + 1)) & (COM_IN_BUF_SIZE - 1);
}

bool com_in_empty(void)
{
    return com_in_head == com_in_tail;
}

// Reports the cursor position
void com_in_write_ansi_CPR(int row, int col)
{
    // If USB terminal connected, let it respond instead of us
    if (!tud_cdc_connected() && com_in_empty())
    {
        snprintf(com_in_buf, COM_IN_BUF_SIZE, "\33[%u;%uR", row, col);
        com_in_tail = COM_OUT_BUF_SIZE - 1;
        com_in_head = strlen(com_in_buf) - 1;
    }
}

void com_in_write(char ch)
{
    com_in_head = (com_in_head + 1) % COM_IN_BUF_SIZE;
    com_in_buf[com_in_head] = ch;
}

bool com_out_empty(void)
{
    return com_out_head == com_out_tail;
}

char com_out_peek(void)
{
    return com_out_buf[(com_out_tail + 1) % COM_OUT_BUF_SIZE];
}

char com_out_read(void)
{
    com_out_tail = (com_out_tail + 1) % COM_OUT_BUF_SIZE;
    return com_out_buf[com_out_tail];
}

static void com_out_chars(const char *buf, int length)
{
    while (length)
    {
        while (length && ((com_out_head + 1) % COM_OUT_BUF_SIZE) != com_out_tail)
        {
            com_out_head = (com_out_head + 1) % COM_OUT_BUF_SIZE;
            com_out_buf[com_out_head] = *buf++;
            length--;
        }
        while (((com_out_head + 1) % COM_OUT_BUF_SIZE) == com_out_tail)
        {
            cdc_task();
            tud_task();
        }
    }
}

void com_init(void)
{
    gpio_set_function(COM_UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(COM_UART_RX_PIN, GPIO_FUNC_UART);
    uart_init(COM_UART_INTERFACE, COM_UART_BAUDRATE);

    static stdio_driver_t stdio_driver = {
        .out_chars = com_out_chars,
        .crlf_enabled = true,
    };
    stdio_set_driver_enabled(&stdio_driver, true);
}

void com_pre_reclock(void)
{
    while (!com_in_empty() && uart_is_writable(COM_UART_INTERFACE))
    {
        com_in_tail = (com_in_tail + 1) % COM_IN_BUF_SIZE;
        uart_get_hw(COM_UART_INTERFACE)->dr = com_in_buf[com_in_tail];
    }
}

void com_post_reclock(void)
{
    uart_init(COM_UART_INTERFACE, COM_UART_BAUDRATE);
}

void com_set_uart_break(bool en)
{
    uart_set_break(COM_UART_INTERFACE, en);
}

void com_task(void)
{
    // IN is sunk here to UART
    while (!com_in_empty() && uart_is_writable(COM_UART_INTERFACE))
    {
        com_in_tail = (com_in_tail + 1) % COM_IN_BUF_SIZE;
        uart_get_hw(COM_UART_INTERFACE)->dr = com_in_buf[com_in_tail];
    }

    // OUT is sourced here from STD UART
    while (uart_is_readable(COM_UART_INTERFACE))
        putchar_raw(uart_getc(COM_UART_INTERFACE));
}
