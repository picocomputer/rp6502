/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <sys/std.h>

// IN is sourced by USB CDC
// IN is sunk here to UART
size_t std_in_head;
size_t std_in_tail;
uint8_t std_in_buf[STD_IN_BUF_SIZE];
#define STD_IN_BUF(pos) std_in_buf[(pos) & (STD_IN_BUF_SIZE - 1)]

// OUT is sourced here from STD UART or PIX
// OUT is sourced from PIX $F:03
// OUT is sunk here to stdio
// OUT is sunk by USB CDC
size_t std_out_tail;
size_t std_out_head;
uint8_t std_out_buf[STD_OUT_BUF_SIZE];
#define STD_OUT_BUF(pos) std_out_buf[(pos) & (STD_OUT_BUF_SIZE - 1)]

bool is_breaking;

size_t std_in_free(void)
{
    return ((std_in_tail + 0) - (std_in_head + 1)) & (STD_IN_BUF_SIZE - 1);
}

bool std_in_empty(void)
{
    return &STD_IN_BUF(std_in_head) == &STD_IN_BUF(std_in_tail);
}

void std_in_write(char ch)
{
    STD_IN_BUF(++std_in_head) = ch;
}

bool std_out_empty(void)
{
    return &STD_OUT_BUF(std_out_tail) == &STD_OUT_BUF(std_out_head);
}

void std_out_write(char ch)
{
    if (&STD_OUT_BUF(std_out_tail + 1) == &STD_OUT_BUF(std_out_head))
        ++std_out_head;
    STD_OUT_BUF(++std_out_tail) = ch;
    // OUT is sunk here to stdio
    putchar_raw(ch);
}

char std_out_peek(void)
{
    return STD_OUT_BUF(std_out_head + 1);
}

char std_out_read(void)
{
    return STD_OUT_BUF(++std_out_head);
}

void std_init(void)
{
    gpio_set_function(STD_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(STD_UART_RX, GPIO_FUNC_UART);
    uart_init(STD_UART_INTERFACE, STD_UART_BAUDRATE);
}

void std_reclock(void)
{
    uart_init(STD_UART_INTERFACE, STD_UART_BAUDRATE);
}

void std_set_break(bool en)
{
    uart_set_break(STD_UART_INTERFACE, en);
    is_breaking = en;
}

void std_task(void)
{
    // IN is sunk here to UART
    while (!std_in_empty() && uart_is_writable(STD_UART_INTERFACE))
        uart_get_hw(STD_UART_INTERFACE)->dr = STD_IN_BUF(++std_in_tail);

    // OUT is sourced here from STD UART
    while (uart_is_readable(STD_UART_INTERFACE))
        std_out_write(uart_getc(STD_UART_INTERFACE));
}
