/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "sys/com.h"
#include "sys/cpu.h"
#include "sys/pix.h"
#include "sys/ria.h"
#include "sys/vga.h"
#include "pico/stdlib.h"
#include "pico/stdio/driver.h"
#include <stdio.h>

#define COM_BUF_SIZE 256
#define COM_CSI_PARAM_MAX_LEN 16

typedef enum
{
    ansi_state_C0,
    ansi_state_Fe,
    ansi_state_SS2,
    ansi_state_SS3,
    ansi_state_CSI
} ansi_state_t;

static char com_buf[COM_BUF_SIZE];
static com_read_callback_t com_callback;
static uint8_t *com_binary_buf;
static absolute_time_t com_timer;
static uint32_t com_timeout_ms;
static size_t com_bufsize;
static size_t com_buflen;
static size_t com_bufpos;
static ansi_state_t com_ansi_state;
static uint16_t com_csi_param[COM_CSI_PARAM_MAX_LEN];
static uint8_t com_csi_param_count;
static stdio_driver_t com_stdio_app;
static uint32_t com_ctrl_bits;

volatile size_t com_tx_tail;
volatile size_t com_tx_head;
volatile uint8_t com_tx_buf[32];

static void com_tx_task(void)
{
    // We sacrifice the UART TX FIFO so PIX STDOUT can keep pace.
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

void com_reset(void)
{
    com_callback = NULL;
    com_binary_buf = NULL;
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

void com_reclock(void)
{
    uart_init(COM_UART, COM_UART_BAUD_RATE);
}

static void com_line_home(void)
{
    if (com_bufpos)
        printf("\33[%dD", com_bufpos);
    com_bufpos = 0;
}

static void com_line_end(void)
{
    if (com_bufpos != com_buflen)
        printf("\33[%dC", com_buflen - com_bufpos);
    com_bufpos = com_buflen;
}

static void com_line_forward_word(void)
{
    int count = 0;
    if (com_bufpos < com_buflen)
        while (true)
        {
            count++;
            if (++com_bufpos >= com_buflen)
                break;
            if (com_buf[com_bufpos] == ' ' && com_buf[com_bufpos - 1] != ' ')
                break;
        }
    if (count)
        printf("\33[%dC", count);
}

static void com_line_forward(void)
{
    uint16_t count = com_csi_param[0];
    if (count < 1)
        count = 1;
    if (com_csi_param_count > 1 && !!(com_csi_param[1] - 1))
        return com_line_forward_word();
    if (count > com_buflen - com_bufpos)
        count = com_buflen - com_bufpos;
    if (!count)
        return;
    com_bufpos += count;
    printf("\33[%dC", count);
}

static void com_line_forward_1(void)
{
    com_csi_param_count = 1;
    com_csi_param[0] = 1;
    com_line_forward();
}

static void com_line_backward_word(void)
{
    int count = 0;
    if (com_bufpos)
        while (true)
        {
            count++;
            if (!--com_bufpos)
                break;
            if (com_buf[com_bufpos] != ' ' && com_buf[com_bufpos - 1] == ' ')
                break;
        }
    if (count)
        printf("\33[%dD", count);
}

static void com_line_backward(void)
{
    uint16_t count = com_csi_param[0];
    if (count < 1)
        count = 1;
    if (com_csi_param_count > 1 && !!(com_csi_param[1] - 1))
        return com_line_backward_word();
    if (count > com_bufpos)
        count = com_bufpos;
    if (!count)
        return;
    com_bufpos -= count;
    printf("\33[%dD", count);
}

static void com_line_backward_1(void)
{
    com_csi_param_count = 1;
    com_csi_param[0] = 1;
    com_line_backward();
}

static void com_line_delete(void)
{
    if (!com_buflen || com_bufpos == com_buflen)
        return;
    printf("\33[P");
    com_buflen--;
    for (uint8_t i = com_bufpos; i < com_buflen; i++)
        com_buf[i] = com_buf[i + 1];
}

static void com_line_backspace(void)
{
    if (!com_bufpos)
        return;
    printf("\b\33[P");
    com_buflen--;
    for (uint8_t i = --com_bufpos; i < com_buflen; i++)
        com_buf[i] = com_buf[i + 1];
}

static void com_line_insert(char ch)
{
    if (ch < 32 || com_buflen >= com_bufsize - 1)
        return;
    for (size_t i = com_buflen; i > com_bufpos; i--)
        com_buf[i] = com_buf[i - 1];
    com_buflen++;
    com_buf[com_bufpos] = ch;
    for (size_t i = com_bufpos; i < com_buflen; i++)
        putchar(com_buf[i]);
    com_bufpos++;
    if (com_buflen - com_bufpos)
        printf("\33[%dD", com_buflen - com_bufpos);
}

static void com_line_state_C0(char ch)
{
    if (com_ctrl_bits & (1 << ch))
    {
        printf("\n");
        com_flush();
        com_buf[0] = ch;
        com_buf[1] = 0;
        com_buflen = 1;
        com_read_callback_t cc = com_callback;
        com_callback = NULL;
        cc(false, com_buf, com_buflen);
    }
    else if (ch == '\r')
    {
        printf("\n");
        com_flush();
        com_buf[com_buflen] = 0;
        com_read_callback_t cc = com_callback;
        com_callback = NULL;
        cc(false, com_buf, com_buflen);
    }
    else if (ch == '\33')
        com_ansi_state = ansi_state_Fe;
    else if (ch == '\b' || ch == 127)
        com_line_backspace();
    else if (ch == 1) // ctrl-a
        com_line_home();
    else if (ch == 2) // ctrl-b
        com_line_backward_1();
    else if (ch == 5) // ctrl-e
        com_line_end();
    else if (ch == 6) // ctrl-f
        com_line_forward_1();
    else
        com_line_insert(ch);
}

static void com_line_state_Fe(char ch)
{
    if (ch == '[')
    {
        com_ansi_state = ansi_state_CSI;
        com_csi_param_count = 0;
        com_csi_param[0] = 0;
    }
    else if (ch == 'b' || ch == 2)
    {
        com_ansi_state = ansi_state_C0;
        com_line_backward_word();
    }
    else if (ch == 'f' || ch == 6)
    {
        com_ansi_state = ansi_state_C0;
        com_line_forward_word();
    }
    else if (ch == 'N')
        com_ansi_state = ansi_state_SS2;
    else if (ch == 'O')
        com_ansi_state = ansi_state_SS3;
    else
    {
        com_ansi_state = ansi_state_C0;
        if (ch == 127)
            com_line_delete();
    }
}

static void com_line_state_SS2(char ch)
{
    (void)ch;
    com_ansi_state = ansi_state_C0;
}

static void com_line_state_SS3(char ch)
{
    com_ansi_state = ansi_state_C0;
    if (ch == 'F')
        com_line_end();
    else if (ch == 'H')
        com_line_home();
}

static void com_line_state_CSI(char ch)
{
    // Silently discard overflow parameters but still count to + 1.
    if (ch >= '0' && ch <= '9')
    {
        if (com_csi_param_count < COM_CSI_PARAM_MAX_LEN)
        {
            com_csi_param[com_csi_param_count] *= 10;
            com_csi_param[com_csi_param_count] += ch - '0';
        }
        return;
    }
    if (ch == ';' || ch == ':')
    {
        if (++com_csi_param_count < COM_CSI_PARAM_MAX_LEN)
            com_csi_param[com_csi_param_count] = 0;
        else
            com_csi_param_count = COM_CSI_PARAM_MAX_LEN;
        return;
    }
    com_ansi_state = ansi_state_C0;
    if (++com_csi_param_count > COM_CSI_PARAM_MAX_LEN)
        com_csi_param_count = COM_CSI_PARAM_MAX_LEN;
    if (ch == 'C')
        com_line_forward();
    else if (ch == 'D')
        com_line_backward();
    else if (ch == 'F')
        com_line_end();
    else if (ch == 'H')
        com_line_home();
    else if (ch == 'b' || ch == 2)
        com_line_backward_word();
    else if (ch == 'f' || ch == 6)
        com_line_forward_word();
    else if (ch == '~')
        switch (com_csi_param[0])
        {
        case 1:
        case 7:
            return com_line_home();
        case 4:
        case 8:
            return com_line_end();
        case 3:
            return com_line_delete();
        }
}

static void com_line_rx(uint8_t ch)
{
    if (ch == '\30')
        com_ansi_state = ansi_state_C0;
    else
        switch (com_ansi_state)
        {
        case ansi_state_C0:
            com_line_state_C0(ch);
            break;
        case ansi_state_Fe:
            com_line_state_Fe(ch);
            break;
        case ansi_state_SS2:
            com_line_state_SS2(ch);
            break;
        case ansi_state_SS3:
            com_line_state_SS3(ch);
            break;
        case ansi_state_CSI:
            com_line_state_CSI(ch);
            break;
        }
}

static void com_binary_rx(uint8_t ch)
{
    com_binary_buf[com_buflen] = ch;
    if (++com_buflen == com_bufsize)
    {
        com_read_callback_t cc = com_callback;
        com_callback = NULL;
        cc(false, (char *)com_binary_buf, com_buflen);
        com_binary_buf = NULL;
    }
}

void com_read_binary(uint32_t timeout_ms, com_read_callback_t callback, uint8_t *buf, size_t size)
{
    com_binary_buf = buf;
    com_bufsize = size;
    com_buflen = 0;
    com_timeout_ms = timeout_ms;
    com_timer = delayed_by_ms(get_absolute_time(), com_timeout_ms);
    com_callback = callback;
}

void com_read_line(uint32_t timeout_ms, com_read_callback_t callback, size_t size, uint32_t ctrl_bits)
{
    com_bufsize = size;
    if (com_bufsize > COM_BUF_SIZE)
        com_bufsize = COM_BUF_SIZE;
    com_buflen = 0;
    com_bufpos = 0;
    com_ansi_state = ansi_state_C0;
    com_timeout_ms = timeout_ms;
    com_timer = delayed_by_ms(get_absolute_time(), com_timeout_ms);
    com_callback = callback;
    com_ctrl_bits = ctrl_bits;
}

void com_task(void)
{
    com_tx_task();

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
    if (!ria_active())
    {
        if (com_callback && com_timeout_ms && absolute_time_diff_us(get_absolute_time(), com_timer) < 0)
        {
            com_read_callback_t cc = com_callback;
            com_callback = NULL;
            com_binary_buf = NULL;
            cc(true, NULL, 0);
        }
        else
        {
            int ch;
            if (cpu_active() && com_callback)
                ch = cpu_getchar();
            else
                ch = getchar_timeout_us(0);
            while (ch != PICO_ERROR_TIMEOUT)
            {
                if (com_callback)
                {
                    com_timer = delayed_by_ms(get_absolute_time(), com_timeout_ms);
                    if (com_binary_buf)
                        com_binary_rx(ch);
                    else
                        com_line_rx(ch);
                }
                else if (cpu_active())
                    cpu_com_rx(ch);
                if (ria_active()) // why?
                    break;
                ch = getchar_timeout_us(0);
            }
        }
    }
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

// Below is mostly the same as the Pi Pico SDK.
// We need lower level access to the out_chars
// so it can be used for pacing PIX STDOUT.
// If the Pi Pico SDK authors read this, what you made
// is generally good but please expose the callbacks.

#if PICO_STDIO_UART_SUPPORT_CHARS_AVAILABLE_CALLBACK
static void (*chars_available_callback)(void *);
static void *chars_available_param;

static void on_uart_rx(void)
{
    if (chars_available_callback)
    {
        // Interrupts will go off until the uart is read, so disable them
        uart_set_irq_enables(COM_UART, false, false);
        chars_available_callback(chars_available_param);
    }
}

static void stdio_uart_set_chars_available_callback(void (*fn)(void *), void *param)
{
    static_assert(UART1_IRQ == UART0_IRQ + 1, "");
    const uint UART_IRQ = UART0_IRQ + uart_get_index(COM_UART);
    if (fn && !chars_available_callback)
    {
        irq_set_exclusive_handler(UART_IRQ, on_uart_rx);
        irq_set_enabled(UART_IRQ, true);
        uart_set_irq_enables(COM_UART, true, false);
    }
    else if (!fn && chars_available_callback)
    {
        uart_set_irq_enables(COM_UART, false, false);
        irq_set_enabled(UART_IRQ, false);
        irq_remove_handler(UART_IRQ, on_uart_rx);
    }
    chars_available_callback = fn;
    chars_available_param = param;
}
#endif

static int stdio_uart_in_chars(char *buf, int length)
{
    int i = 0;
    while (i < length && uart_is_readable(COM_UART))
    {
        buf[i++] = uart_getc(COM_UART);
    }
#if PICO_STDIO_UART_SUPPORT_CHARS_AVAILABLE_CALLBACK
    if (chars_available_callback)
    {
        // Re-enable interrupts after reading a character
        uart_set_irq_enables(COM_UART, true, false);
    }
#endif
    return i ? i : PICO_ERROR_NO_DATA;
}

static stdio_driver_t com_stdio_app = {
    .out_chars = com_stdio_out_chars,
    .in_chars = stdio_uart_in_chars,
#if PICO_STDIO_UART_SUPPORT_CHARS_AVAILABLE_CALLBACK
    .set_chars_available_callback = stdio_uart_set_chars_available_callback,
#endif
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
    .crlf_enabled = PICO_STDIO_DEFAULT_CRLF
#endif
};
