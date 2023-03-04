/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "pix.h"
#include "pix.pio.h"
#include "mem/vram.h"
#include "hardware/dma.h"
#include <stdio.h>

#define VGA_PIX_PIO pio1
#define VGA_PIX_SM 1
#define VGA_PHI2_PIN 11

static void pix_vram_pio_init()
{
    static volatile uint8_t *dma_addr;
    dma_addr = vram;

    // PIO to receive PIX graphics bus
    uint offset = pio_add_program(VGA_PIX_PIO, &vga_pix_program);
    pio_sm_config config = vga_pix_program_get_default_config(offset);
    sm_config_set_in_pins(&config, 0);
    sm_config_set_in_shift(&config, false, false, 0);
    pio_sm_init(VGA_PIX_PIO, VGA_PIX_SM, offset, &config);
    pio_sm_put(VGA_PIX_PIO, VGA_PIX_SM, 0x0);
    pio_sm_exec_wait_blocking(VGA_PIX_PIO, VGA_PIX_SM, pio_encode_pull(false, true));
    pio_sm_exec_wait_blocking(VGA_PIX_PIO, VGA_PIX_SM, pio_encode_mov(pio_x, pio_osr));
    pio_sm_set_enabled(VGA_PIX_PIO, VGA_PIX_SM, true);

    // Need both channels now to configure chain ping-pong
    int addr_chan = dma_claim_unused_channel(true);
    int vram_chan = dma_claim_unused_channel(true);
    int data_chan = dma_claim_unused_channel(true);

    // DMA move the requested memory data to PIO for output
    dma_channel_config vram_dma = dma_channel_get_default_config(vram_chan);
    channel_config_set_read_increment(&vram_dma, false);
    channel_config_set_chain_to(&vram_dma, data_chan);
    dma_channel_configure(
        vram_chan,
        &vram_dma,
        &dma_channel_hw_addr(data_chan)->write_addr, // dst
        &dma_addr,                                   // src
        1,
        false);

    // DMA move the requested memory data to PIO for output
    dma_channel_config data_dma = dma_channel_get_default_config(data_chan);
    channel_config_set_dreq(&data_dma, pio_get_dreq(VGA_PIX_PIO, VGA_PIX_SM, false));
    channel_config_set_read_increment(&data_dma, false);
    channel_config_set_transfer_data_size(&data_dma, DMA_SIZE_8);
    channel_config_set_chain_to(&data_dma, addr_chan);
    dma_channel_configure(
        data_chan,
        &data_dma,
        dma_addr,                      // dst
        &VGA_PIX_PIO->rxf[VGA_PIX_SM], // src
        1,
        false);

    // DMA move address from PIO into the data DMA config
    dma_channel_config addr_dma = dma_channel_get_default_config(addr_chan);
    channel_config_set_dreq(&addr_dma, pio_get_dreq(VGA_PIX_PIO, VGA_PIX_SM, false));
    channel_config_set_read_increment(&addr_dma, false);
    channel_config_set_transfer_data_size(&data_dma, DMA_SIZE_16);
    channel_config_set_chain_to(&addr_dma, vram_chan);
    dma_channel_configure(
        addr_chan,
        &addr_dma,
        &dma_addr,                     // dst
        &VGA_PIX_PIO->rxf[VGA_PIX_SM], // src
        1,
        false);
}

void pix_init()
{
    pix_vram_pio_init();
}

void pix_task()
{
    if (!pio_sm_is_rx_fifo_empty(VGA_PIX_PIO, VGA_PIX_SM))
    {
        uint32_t data = pio_sm_get(VGA_PIX_PIO, VGA_PIX_SM);
        printf("Rx: $%lX\n", data);
    }
}
