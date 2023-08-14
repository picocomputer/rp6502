/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ria.h"
#include "sys/std.h"
#include "ria.pio.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include <string.h>

// PIX is unidirectional and we're out of pins.
// The RIA also sends UART data over PIX so we can
// reconfigure that pin for a return channel.
#define BACKCHAN_PIN STD_UART_RX
#define BACKCHAN_BAUDRATE 115200
#define BACKCHAN_PIO pio1
#define BACKCHAN_SM 3
static bool ria_backchan_enabled;

// Weird version logic because C macros are lame
static const char version_dev[] = "\rVGA " __DATE__ " " __TIME__;
static const char version_full[] = "\rVGA Version " RP6502_VERSION;
static const char *version_pos = version_dev;

// TODO Merge this STDOUT with the UART RX STDOUT in cdc.c and
//      get rid of the redundant readable checks and unused blocking.
//      Also get printf() sending to USB for TinyUSB logging.
static size_t ria_stdout_head;
static size_t ria_stdout_tail;
static uint8_t ria_stdout_buf[32];
#define RIA_STDOUT_BUF(pos) ria_stdout_buf[(pos)&0x1F]

void ria_init(void)
{
    gpio_pull_up(BACKCHAN_PIN);
    pio_sm_set_pins_with_mask(BACKCHAN_PIO, BACKCHAN_SM, 1u << BACKCHAN_PIN, 1u << BACKCHAN_PIN);
    pio_sm_set_pindirs_with_mask(BACKCHAN_PIO, BACKCHAN_SM, 1u << BACKCHAN_PIN, 1u << BACKCHAN_PIN);
    uint offset = pio_add_program(BACKCHAN_PIO, &uart_tx_program);
    pio_sm_config c = uart_tx_program_get_default_config(offset);
    sm_config_set_out_shift(&c, true, false, 32);
    sm_config_set_out_pins(&c, BACKCHAN_PIN, 1);
    sm_config_set_sideset_pins(&c, BACKCHAN_PIN);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    float div = (float)clock_get_hz(clk_sys) / (8 * BACKCHAN_BAUDRATE);
    sm_config_set_clkdiv(&c, div);
    pio_sm_init(BACKCHAN_PIO, BACKCHAN_SM, offset, &c);
    pio_sm_set_enabled(BACKCHAN_PIO, BACKCHAN_SM, true);
}

void ria_task(void)
{
    char ch = *version_pos;
    if (ch != '\r' && !pio_sm_is_tx_fifo_full(BACKCHAN_PIO, BACKCHAN_SM))
    {
        if (ch)
            version_pos++;
        else
        {
            version_pos = version_dev;
            ch = *version_pos;
        }
        pio_sm_put(BACKCHAN_PIO, BACKCHAN_SM, ch);
    }
}

void ria_reclock(void)
{
    float div = (float)clock_get_hz(clk_sys) / (8 * BACKCHAN_BAUDRATE);
    pio_sm_set_clkdiv_int_frac(BACKCHAN_PIO, BACKCHAN_SM, div, 0);
}

void ria_backchan(uint16_t word)
{
    switch (word)
    {
    case 0: // off
        ria_backchan_enabled = false;
        gpio_set_function(BACKCHAN_PIN, GPIO_FUNC_UART);
        break;
    case 1: // on
        ria_backchan_enabled = true;
        pio_gpio_init(BACKCHAN_PIO, BACKCHAN_PIN);
        if (strlen(RP6502_VERSION))
            version_pos = version_full + 1;
        else
            version_pos = version_dev + 1;
        break;
    case 2: // send ack
        uart_write_blocking(STD_UART_INTERFACE, "VGA1", 4);
        break;
    }
}

bool ria_backchannel(void)
{
    return ria_backchan_enabled;
}

void ria_stdout_rx(char ch)
{
    if (!ria_backchan_enabled)
        return;
    if (&RIA_STDOUT_BUF(ria_stdout_tail + 1) != &RIA_STDOUT_BUF(ria_stdout_head))
        RIA_STDOUT_BUF(++ria_stdout_tail) = ch;
}

bool ria_stdout_is_readable(void)
{
    return &RIA_STDOUT_BUF(ria_stdout_tail) != &RIA_STDOUT_BUF(ria_stdout_head);
}

char ria_stdout_getc(void)
{
    return RIA_STDOUT_BUF(++ria_stdout_head);
}

void ria_vsync(void)
{
    static uint32_t frame_no;
    if (ria_backchan_enabled)
        pio_sm_put(BACKCHAN_PIO, BACKCHAN_SM, ++frame_no | 0x80);
}
