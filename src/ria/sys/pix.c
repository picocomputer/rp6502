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

bool pix_reset_requested;

void pix_task()
{
    if (pix_reset_requested)
    {
        pix_reset_requested = false;
        uint16_t config_bits = cfg_get_vga();
        for (uint8_t dev = 1; dev < 7; dev++)
            pix_send_blocking(dev, 0xFu, 0xFFu, config_bits);
    }
}

void pix_stop()
{
    pix_reset_requested = true;
}

bool pix_set_vga(uint32_t disp)
{
    (void)disp;
    pix_reset_requested = true;
    return true;
}

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
    pix_reset_requested = true;
}

void pix_api_set_xreg()
{
    unsigned dev = API_A & 0x7;
    if (xstack_ptr < XSTACK_SIZE - 4 || xstack_ptr > XSTACK_SIZE - 3)
        return api_return_errno_ax(FR_INVALID_PARAMETER, -1);
    uint16_t byte = xstack[xstack_ptr];
    xstack_ptr += 2;
    uint16_t word = api_sstack_uint16();
    if (xstack_ptr != XSTACK_SIZE)
        return api_return_errno_ax(FR_INVALID_PARAMETER, -1);
    uint8_t ch = (byte >> 8) & 0xF;
    if (!dev)
        main_pix(ch, byte, word);
    else
        pix_send_blocking(dev, ch, byte, word);
    return api_return_ax(0);
}
