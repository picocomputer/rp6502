/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ria.h"
#include "usb/cdc.h"
#include "ria.pio.h"
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"

// PIX is unidirectional and we're out of pins.
// The RIA also sends UART data over PIX so we can
// reconfigure that pin for a return channel.
#define BACKCHAN_PIN PICOPROBE_UART_RX
#define BACKCHAN_BAUDRATE 115200
#define BACKCHAN_PIO pio1
#define BACKCHAN_SM 3

void ria_init(void)
{
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

void ria_reclock(void)
{
    float div = (float)clock_get_hz(clk_sys) / (8 * BACKCHAN_BAUDRATE);
    pio_sm_set_clkdiv_int_frac(BACKCHAN_PIO, BACKCHAN_SM, div, 0);
}

void ria_backchan_req(void)
{
    uart_write_blocking(PICOPROBE_UART_INTERFACE, "VGA1\r", 5);
}

void ria_backchan_ack(void)
{
    pio_gpio_init(BACKCHAN_PIO, BACKCHAN_PIN);
}

void ria_vsync(void)
{
    static uint32_t frame_no;
    pio_sm_put(BACKCHAN_PIO, BACKCHAN_SM, ++frame_no);
}
