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
#include "pico/stdlib.h"

static uint32_t pix_send_count;
static bool pix_wait_for_vga_ack;
static absolute_time_t pix_ack_timer;

#define PIX_ACK_TIMEOUT_MS 2

void pix_reclock(uint16_t clkdiv_int, uint8_t clkdiv_frac)
{
    pio_sm_set_clkdiv_int_frac(PIX_PIO, PIX_SM, clkdiv_int, clkdiv_frac);
}

void pix_init(void)
{
    uint offset = pio_add_program(PIX_PIO, &pix_send_program);
    pio_sm_config config = pix_send_program_get_default_config(offset);
    sm_config_set_out_pins(&config, PIX_PIN_BASE, 4);
    sm_config_set_out_shift(&config, false, false, 32);
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);
    for (int i = 0; i < 4; i++)
        pio_gpio_init(PIX_PIO, i);
    pio_sm_set_consecutive_pindirs(PIX_PIO, PIX_SM, PIX_PIN_BASE, 4, true);
    pio_sm_init(PIX_PIO, PIX_SM, offset, &config);
    pio_sm_put(PIX_PIO, PIX_SM, PIX_MESSAGE(PIX_DEVICE_IDLE, 0, 0, 0));
    pio_sm_exec_wait_blocking(PIX_PIO, PIX_SM, pio_encode_pull(false, true));
    pio_sm_exec_wait_blocking(PIX_PIO, PIX_SM, pio_encode_mov(pio_x, pio_osr));
    pio_sm_set_enabled(PIX_PIO, PIX_SM, true);

    // Queue a couple sync frames for safety
    pix_send(PIX_DEVICE_IDLE, 0, 0, 0);
    pix_send(PIX_DEVICE_IDLE, 0, 0, 0);
}

void pix_stop(void)
{
    pix_wait_for_vga_ack = false;
    pix_send_count = 0;
}

void pix_ack(void)
{
    pix_wait_for_vga_ack = false;
    if (pix_send_count == 0)
    {
        api_zxstack();
        return api_return_ax(0);
    }
}

void pix_nak(void)
{
    pix_wait_for_vga_ack = false;
    pix_send_count = 0;
    return api_return_errno(API_EINVAL);
}

void pix_api_xreg(void)
{
    static uint8_t pix_device;
    static uint8_t pix_channel;
    static uint8_t pix_addr;

    // Check for timeout
    if (pix_wait_for_vga_ack)
    {
        if (absolute_time_diff_us(get_absolute_time(), pix_ack_timer) < 0)
        {
            pix_wait_for_vga_ack = false;
            pix_send_count = 0;
            return api_return_errno(API_EIO);
        }
        return;
    }

    // In progress, send one xreg
    if (pix_send_count)
    {
        if (pix_ready())
        {
            --pix_send_count;
            uint16_t data;
            api_pop_uint16(&data);
            pix_send(pix_device, pix_channel, pix_addr + pix_send_count, data);
            if (pix_device == PIX_DEVICE_VGA && pix_channel == 0 &&
                pix_addr + pix_send_count <= 1)
            {
                pix_wait_for_vga_ack = true;
                pix_ack_timer = make_timeout_time_ms(PIX_ACK_TIMEOUT_MS);
            }
            else if (!pix_send_count)
            {
                api_zxstack();
                return api_return_ax(0);
            }
        }
        return;
    }

    // Setup for new call
    pix_device = xstack[XSTACK_SIZE - 1];
    pix_channel = xstack[XSTACK_SIZE - 2];
    pix_addr = xstack[XSTACK_SIZE - 3];
    pix_send_count = (XSTACK_SIZE - xstack_ptr - 3) / 2;
    if (!(xstack_ptr & 0x01) ||
        pix_send_count < 1 || pix_send_count > XSTACK_SIZE / 2 ||
        pix_device > 7 || pix_channel > 15)
    {
        pix_send_count = 0;
        return api_return_errno(API_EINVAL);
    }

    // Local PIX device $0
    if (pix_device == PIX_DEVICE_RIA)
    {
        for (; pix_send_count; pix_send_count--)
        {
            uint16_t data;
            api_pop_uint16(&data);
            if (!main_pix(pix_channel, pix_addr, data))
            {
                pix_send_count = 0;
                return api_return_errno(API_EINVAL);
            }
        }
        api_zxstack();
        return api_return_ax(0);
    }

    // Special case of sending VGA canvas and mode in same call.
    // Because we send in reverse, canvas has to be first or it'll clear mode programming.
    if (pix_device == PIX_DEVICE_VGA && pix_channel == 0 &&
        pix_addr == 0 && pix_send_count > 1)
    {
        pix_send_blocking(PIX_DEVICE_VGA, 0, 0, *(uint16_t *)&xstack[XSTACK_SIZE - 5]);
        pix_addr = 1;
        pix_send_count -= 1;
        pix_wait_for_vga_ack = true;
        pix_ack_timer = make_timeout_time_ms(PIX_ACK_TIMEOUT_MS);
    }
}
