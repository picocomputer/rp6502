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
#define VGA_PIX_REGS_SM 1
#define VGA_PIX_VRAM_SM 2
#define VGA_PHI2_PIN 11

void pix_init()
{
    static volatile uint8_t dma_fifo[4];
    static volatile uint32_t dma_addr;
    dma_addr = (uint32_t)vram;

    // Two state machines, one program
    uint offset = pio_add_program(VGA_PIX_PIO, &vga_pix_program);

    // PIO to receive VGA registers
    pio_sm_config regs_config = vga_pix_program_get_default_config(offset);
    sm_config_set_in_pins(&regs_config, 0);
    sm_config_set_in_shift(&regs_config, false, false, 0);
    sm_config_set_out_shift(&regs_config, true, false, 4);
    pio_sm_init(VGA_PIX_PIO, VGA_PIX_REGS_SM, offset, &regs_config);
    pio_sm_put(VGA_PIX_PIO, VGA_PIX_REGS_SM, 0x0); // channel 0
    pio_sm_exec_wait_blocking(VGA_PIX_PIO, VGA_PIX_REGS_SM, pio_encode_pull(false, true));
    pio_sm_exec_wait_blocking(VGA_PIX_PIO, VGA_PIX_REGS_SM, pio_encode_mov(pio_x, pio_osr));
    pio_sm_exec_wait_blocking(VGA_PIX_PIO, VGA_PIX_REGS_SM, pio_encode_out(pio_null, 32));
    pio_sm_set_enabled(VGA_PIX_PIO, VGA_PIX_REGS_SM, true);

    // PIO to receive VRAM
    pio_sm_config vram_config = vga_pix_program_get_default_config(offset);
    sm_config_set_in_pins(&vram_config, 0);
    sm_config_set_in_shift(&vram_config, false, false, 0);
    sm_config_set_out_shift(&vram_config, true, false, 4);
    pio_sm_init(VGA_PIX_PIO, VGA_PIX_VRAM_SM, offset, &vram_config);
    pio_sm_put(VGA_PIX_PIO, VGA_PIX_VRAM_SM, 0x1); // channel 1
    pio_sm_exec_wait_blocking(VGA_PIX_PIO, VGA_PIX_VRAM_SM, pio_encode_pull(false, true));
    pio_sm_exec_wait_blocking(VGA_PIX_PIO, VGA_PIX_VRAM_SM, pio_encode_mov(pio_x, pio_osr));
    pio_sm_exec_wait_blocking(VGA_PIX_PIO, VGA_PIX_VRAM_SM, pio_encode_out(pio_null, 32));
    pio_sm_set_enabled(VGA_PIX_PIO, VGA_PIX_VRAM_SM, true);

    // Need all channels now to configure chaining
    int copy_chan = dma_claim_unused_channel(true);
    int addr_chan = dma_claim_unused_channel(true);
    int data_chan = dma_claim_unused_channel(true);
    int fifo_chan = dma_claim_unused_channel(true);

    // DMA move the requested memory data to PIO for output
    dma_channel_config copy_dma = dma_channel_get_default_config(copy_chan);
    channel_config_set_transfer_data_size(&copy_dma, DMA_SIZE_16);
    channel_config_set_read_increment(&copy_dma, false);
    channel_config_set_chain_to(&copy_dma, addr_chan);
    dma_channel_configure(
        copy_chan,
        &copy_dma,
        &dma_addr,    // dst
        &dma_fifo[0], // src
        1,
        false);

    // DMA move the requested memory data to PIO for output
    dma_channel_config addr_dma = dma_channel_get_default_config(addr_chan);
    channel_config_set_read_increment(&addr_dma, false);
    channel_config_set_chain_to(&addr_dma, data_chan);
    dma_channel_configure(
        addr_chan,
        &addr_dma,
        &dma_channel_hw_addr(data_chan)->write_addr, // dst
        &dma_addr,                                   // src
        1,
        false);

    // DMA move the requested memory data to PIO for output
    dma_channel_config data_dma = dma_channel_get_default_config(data_chan);
    channel_config_set_read_increment(&data_dma, false);
    channel_config_set_transfer_data_size(&data_dma, DMA_SIZE_8);
    channel_config_set_chain_to(&data_dma, fifo_chan);
    dma_channel_configure(
        data_chan,
        &data_dma,
        vram,         // dst
        &dma_fifo[2], // src
        1,
        false);

    // DMA move address from PIO into the data DMA config
    dma_channel_config fifo_dma = dma_channel_get_default_config(fifo_chan);
    channel_config_set_dreq(&fifo_dma, pio_get_dreq(VGA_PIX_PIO, VGA_PIX_VRAM_SM, false));
    channel_config_set_read_increment(&fifo_dma, false);
    channel_config_set_chain_to(&fifo_dma, copy_chan);
    dma_channel_configure(
        fifo_chan,
        &fifo_dma,
        &dma_fifo[0],                       // dst
        &VGA_PIX_PIO->rxf[VGA_PIX_VRAM_SM], // src
        1,
        true);
}

void pix_task()
{
    if (!pio_sm_is_rx_fifo_empty(VGA_PIX_PIO, VGA_PIX_REGS_SM))
    {
        uint32_t raw = pio_sm_get(VGA_PIX_PIO, VGA_PIX_REGS_SM);
        uint16_t data = raw;
        // 0-0xFF reachable from api_set_vreg
        // 0x100-0xFFF for internal RIA-to-VGA
        uint16_t command = (raw & 0xFFF0000) >> 16;
        switch (command)
        {
        case 0x0FF:
            printf("VRAM $%04X $%02X\n", data, vram[data]);
            break;
        default:
#ifndef NDEBUG
            printf("VREG: $%02X $%04X\n", command, data);
#endif
            break;
        }
    }
}
