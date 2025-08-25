/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sys/com.h"
#include <tusb.h>
#include <pico/stdlib.h>

size_t com_in_head;
size_t com_in_tail;
char com_in_buf[COM_IN_BUF_SIZE];
#define COM_IN_BUF(pos) com_in_buf[(pos) % COM_IN_BUF_SIZE]

size_t com_out_tail;
size_t com_out_head;
char com_out_buf[COM_OUT_BUF_SIZE];
#define COM_OUT_BUF(pos) com_out_buf[(pos) % COM_OUT_BUF_SIZE]

size_t com_in_free(void)
{
    return ((com_in_tail + 0) - (com_in_head + 1)) & (COM_IN_BUF_SIZE - 1);
}

bool com_in_empty(void)
{
    return &COM_IN_BUF(com_in_head) == &COM_IN_BUF(com_in_tail);
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
    COM_IN_BUF(++com_in_head) = ch;
}

bool com_out_empty(void)
{
    return &COM_OUT_BUF(com_out_tail) == &COM_OUT_BUF(com_out_head);
}

void com_out_write(char ch)
{
    if (&COM_OUT_BUF(com_out_tail + 1) == &COM_OUT_BUF(com_out_head))
        ++com_out_head;
    COM_OUT_BUF(++com_out_tail) = ch;
    // OUT is sunk here to stdio
    putchar_raw(ch);
}

char com_out_peek(void)
{
    return COM_OUT_BUF(com_out_head + 1);
}

char com_out_read(void)
{
    return COM_OUT_BUF(++com_out_head);
}

void com_init(void)
{
    gpio_set_function(COM_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(COM_UART_RX, GPIO_FUNC_UART);
    uart_init(COM_UART_INTERFACE, COM_UART_BAUDRATE);
}

void com_flush(void)
{
    while (!com_in_empty() && uart_is_writable(COM_UART_INTERFACE))
        uart_get_hw(COM_UART_INTERFACE)->dr = COM_IN_BUF(++com_in_tail);
}

void com_reclock(void)
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
        uart_get_hw(COM_UART_INTERFACE)->dr = COM_IN_BUF(++com_in_tail);

    // OUT is sourced here from STD UART
    while (uart_is_readable(COM_UART_INTERFACE))
        com_out_write(uart_getc(COM_UART_INTERFACE));
}
