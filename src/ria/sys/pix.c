/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "api/api.h"
#include "sys/cfg.h"
#include "sys/pix.h"
#include "sys.pio.h"
#include "fatfs/ff.h"

void pix_reclock(uint16_t clkdiv_int, uint8_t clkdiv_frac)
{
    pio_sm_set_clkdiv_int_frac(PIX_PIO, PIX_SM, clkdiv_int, clkdiv_frac);
}

void pix_init()
{
    uint offset = pio_add_program(PIX_PIO, &pix_send_program);
    pio_sm_config config = pix_send_program_get_default_config(offset);
    sm_config_set_out_pins(&config, 0, 4);
    sm_config_set_out_shift(&config, false, false, 32);
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);
    for (int i = 0; i < 4; i++)
        pio_gpio_init(PIX_PIO, i);
    pio_sm_set_consecutive_pindirs(PIX_PIO, PIX_SM, 0, 4, true);
    pio_sm_init(PIX_PIO, PIX_SM, offset, &config);
    pio_sm_put(PIX_PIO, PIX_SM, PIX_MESSAGE(PIX_IDLE_DEV, 0, 0, 0));
    pio_sm_exec_wait_blocking(PIX_PIO, PIX_SM, pio_encode_pull(false, true));
    pio_sm_exec_wait_blocking(PIX_PIO, PIX_SM, pio_encode_mov(pio_x, pio_osr));
    pio_sm_set_enabled(PIX_PIO, PIX_SM, true);

    // Queue a couple sync frames for safety
    pix_send(PIX_IDLE_DEV, 0, 0, 0);
    pix_send(PIX_IDLE_DEV, 0, 0, 0);
}

void pix_api_set_xreg()
{
    uint8_t device = xstack[XSTACK_SIZE - 1];
    uint8_t channel = xstack[XSTACK_SIZE - 2];
    uint8_t addr = xstack[XSTACK_SIZE - 3];
    uint32_t count = (XSTACK_SIZE - xstack_ptr - 3) / 2;
    if (!(xstack_ptr & 0x01) ||
        count < 1 || count > XSTACK_SIZE / 2 ||
        device > 7 || channel > 15)
        return api_return_errno(API_EINVAL);
    while (xstack_ptr < XSTACK_SIZE - 3)
    {
        uint16_t data;
        api_pop_uint16(&data);
        pix_send_blocking(device, channel, addr, data);
    }
    return api_return_ax(0);
}
