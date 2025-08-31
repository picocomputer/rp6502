/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "vga.pio.h"
#include "sys/com.h"
#include "sys/ria.h"
#include "sys/sys.h"
#include <pico/stdlib.h>
#include <hardware/clocks.h>
#include <string.h>
#include <stdio.h>

// PIX is unidirectional and we're out of pins.
// The RIA also sends UART data over PIX so we can
// reconfigure that pin for a return channel.
#define RIA_BACKCHAN_PIN COM_UART_RX
#define RIA_BACKCHAN_BAUDRATE 115200
#define RIA_BACKCHAN_PIO pio1
#define RIA_BACKCHAN_SM 3

static const char *version_pos;

void ria_init(void)
{
    gpio_pull_up(RIA_BACKCHAN_PIN);
    pio_sm_set_pins_with_mask(RIA_BACKCHAN_PIO, RIA_BACKCHAN_SM, 1u << RIA_BACKCHAN_PIN, 1u << RIA_BACKCHAN_PIN);
    pio_sm_set_pindirs_with_mask(RIA_BACKCHAN_PIO, RIA_BACKCHAN_SM, 1u << RIA_BACKCHAN_PIN, 1u << RIA_BACKCHAN_PIN);
    uint offset = pio_add_program(RIA_BACKCHAN_PIO, &ria_backchannel_tx_program);
    pio_sm_config c = ria_backchannel_tx_program_get_default_config(offset);
    sm_config_set_out_shift(&c, true, false, 32);
    sm_config_set_out_pins(&c, RIA_BACKCHAN_PIN, 1);
    sm_config_set_sideset_pins(&c, RIA_BACKCHAN_PIN);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    float div = (float)clock_get_hz(clk_sys) / (8 * RIA_BACKCHAN_BAUDRATE);
    sm_config_set_clkdiv(&c, div);
    pio_sm_init(RIA_BACKCHAN_PIO, RIA_BACKCHAN_SM, offset, &c);
    pio_sm_set_enabled(RIA_BACKCHAN_PIO, RIA_BACKCHAN_SM, true);
}

void ria_task(void)
{
    if (version_pos && pio_sm_is_tx_fifo_empty(RIA_BACKCHAN_PIO, RIA_BACKCHAN_SM))
    {
        char ch = *version_pos++;
        if (!ch)
        {
            ch = '\r';
            version_pos = NULL;
        }
        pio_sm_put(RIA_BACKCHAN_PIO, RIA_BACKCHAN_SM, ch);
    }
}

void ria_pre_reclock(void)
{
    // Wait for empty FIFO
    while (pio_sm_get_tx_fifo_level(RIA_BACKCHAN_PIO, RIA_BACKCHAN_SM))
        tight_loop_contents();
    // Wait for shift register too
    float clock = (float)clock_get_hz(clk_sys);
    busy_wait_us_32(clock / RIA_BACKCHAN_BAUDRATE / clock * 10 * 1000000);
}

void ria_post_reclock(void)
{
    float div = (float)clock_get_hz(clk_sys) / (8 * RIA_BACKCHAN_BAUDRATE);
    pio_sm_set_clkdiv_int_frac(RIA_BACKCHAN_PIO, RIA_BACKCHAN_SM, div, 0);
}

void ria_backchan(uint16_t word)
{
    switch (word)
    {
    case 0: // disable
        gpio_set_function(RIA_BACKCHAN_PIN, GPIO_FUNC_UART);
        break;
    case 1: // enable
        pio_gpio_init(RIA_BACKCHAN_PIO, RIA_BACKCHAN_PIN);
        version_pos = sys_version();
        break;
    case 2: // request
        uart_write_blocking(COM_UART_INTERFACE, (uint8_t *)"VGA1", 4);
        break;
    }
}

void ria_vsync(void)
{
    static uint32_t frame_no;
    pio_sm_put(RIA_BACKCHAN_PIO, RIA_BACKCHAN_SM, (++frame_no & 0xF) | 0x80);
}

void ria_ack(void)
{
    pio_sm_put(RIA_BACKCHAN_PIO, RIA_BACKCHAN_SM, 0x90);
}

void ria_nak(void)
{
    pio_sm_put(RIA_BACKCHAN_PIO, RIA_BACKCHAN_SM, 0xA0);
}
