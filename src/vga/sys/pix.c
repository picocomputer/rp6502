/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "sys/pix.h"
#include "sys/ria.h"
#include "sys/std.h"
#include "sys/vga.h"
#include "pix.pio.h"
#include "sys/mem.h"
#include "term/font.h"
#include "hardware/dma.h"
#include "hardware/structs/bus_ctrl.h"
#include <string.h>

#define VGA_PIX_PIO pio1
#define VGA_PIX_REGS_SM 1
#define VGA_PIX_XRAM_SM 2
#define VGA_PHI2_PIN 11

#define PIX_CH0_XREGS_MAX 8
static uint16_t xregs[PIX_CH0_XREGS_MAX];

static bool pix_ch0_xreg(uint8_t addr, uint16_t word)
{
    if (addr < PIX_CH0_XREGS_MAX)
        xregs[addr] = word;
    if (addr == 0) // CANVAS
        if (vga_xreg_canvas(xregs))
            ria_ack();
        else
            ria_nak();
    if (addr == 1) // MODE
    {
        if (main_prog(xregs))
            ria_ack();
        else
            ria_nak();
    }
    if (addr == 0 || addr == 1)
    {
        memset(&xregs, 0, sizeof(xregs));
        return true;
    }
    return false;
}

static bool pix_ch15_xreg(uint8_t addr, uint16_t word)
{
    switch (addr)
    {
    case 0x00: // DISPLAY
        // Also performs a reset.
        vga_xreg_canvas(NULL);
        vga_set_display(word);
        memset(&xregs, 0, sizeof(xregs));
        return true;
    case 0x01: // CODEPAGE
        font_set_codepage(word);
        return true;
    case 0x03: // UART_TX
        std_out_write(word);
        return false;
    case 0x04: // BACKCHAN
        ria_backchan(word);
        return false;
    }
    return false;
}

void pix_init(void)
{
    static volatile uint8_t dma_fifo[4];
    static volatile uint32_t dma_addr;

    // Raise DMA above CPU on crossbar
    bus_ctrl_hw->priority |=
        BUSCTRL_BUS_PRIORITY_DMA_R_BITS |
        BUSCTRL_BUS_PRIORITY_DMA_W_BITS;

    dma_addr = (uint32_t)xram;

    // Two state machines, one program
    uint offset = pio_add_program(VGA_PIX_PIO, &vga_pix_program);

    // PIO to receive VGA registers
    pio_sm_config regs_config = vga_pix_program_get_default_config(offset);
    sm_config_set_in_pins(&regs_config, 0);
    sm_config_set_in_shift(&regs_config, false, false, 0);
    sm_config_set_out_shift(&regs_config, true, false, 4);
    pio_sm_init(VGA_PIX_PIO, VGA_PIX_REGS_SM, offset, &regs_config);
    pio_sm_put(VGA_PIX_PIO, VGA_PIX_REGS_SM, 0x1); // channel 1
    pio_sm_exec_wait_blocking(VGA_PIX_PIO, VGA_PIX_REGS_SM, pio_encode_pull(false, true));
    pio_sm_exec_wait_blocking(VGA_PIX_PIO, VGA_PIX_REGS_SM, pio_encode_mov(pio_x, pio_osr));
    pio_sm_exec_wait_blocking(VGA_PIX_PIO, VGA_PIX_REGS_SM, pio_encode_out(pio_null, 32));
    sm_config_set_fifo_join(&regs_config, PIO_FIFO_JOIN_RX);
    pio_sm_init(VGA_PIX_PIO, VGA_PIX_REGS_SM, offset, &regs_config);
    pio_sm_set_enabled(VGA_PIX_PIO, VGA_PIX_REGS_SM, true);

    // PIO to receive XRAM
    pio_sm_config xram_config = vga_pix_program_get_default_config(offset);
    sm_config_set_in_pins(&xram_config, 0);
    sm_config_set_in_shift(&xram_config, false, false, 0);
    sm_config_set_out_shift(&xram_config, true, false, 4);
    pio_sm_init(VGA_PIX_PIO, VGA_PIX_XRAM_SM, offset, &xram_config);
    pio_sm_put(VGA_PIX_PIO, VGA_PIX_XRAM_SM, 0x0); // channel 0
    pio_sm_exec_wait_blocking(VGA_PIX_PIO, VGA_PIX_XRAM_SM, pio_encode_pull(false, true));
    pio_sm_exec_wait_blocking(VGA_PIX_PIO, VGA_PIX_XRAM_SM, pio_encode_mov(pio_x, pio_osr));
    pio_sm_exec_wait_blocking(VGA_PIX_PIO, VGA_PIX_XRAM_SM, pio_encode_out(pio_null, 32));
    sm_config_set_fifo_join(&xram_config, PIO_FIFO_JOIN_RX);
    pio_sm_init(VGA_PIX_PIO, VGA_PIX_XRAM_SM, offset, &regs_config);
    pio_sm_set_enabled(VGA_PIX_PIO, VGA_PIX_XRAM_SM, true);

    // Need all channels now to configure chaining
    int copy_chan = dma_claim_unused_channel(true);
    int addr_chan = dma_claim_unused_channel(true);
    int data_chan = dma_claim_unused_channel(true);
    int fifo_chan = dma_claim_unused_channel(true);

    // DMA move the XRAM address to low bytes of a pointer.
    dma_channel_config copy_dma = dma_channel_get_default_config(copy_chan);
    channel_config_set_high_priority(&copy_dma, true);
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

    // DMA move the constructed pointer to the next DMA source
    dma_channel_config addr_dma = dma_channel_get_default_config(addr_chan);
    channel_config_set_high_priority(&addr_dma, true);
    channel_config_set_read_increment(&addr_dma, false);
    channel_config_set_chain_to(&addr_dma, data_chan);
    dma_channel_configure(
        addr_chan,
        &addr_dma,
        &dma_channel_hw_addr(data_chan)->write_addr, // dst
        &dma_addr,                                   // src
        1,
        false);

    // DMA move the XRAM data to its new home
    dma_channel_config data_dma = dma_channel_get_default_config(data_chan);
    channel_config_set_high_priority(&data_dma, true);
    channel_config_set_read_increment(&data_dma, false);
    channel_config_set_transfer_data_size(&data_dma, DMA_SIZE_8);
    channel_config_set_chain_to(&data_dma, fifo_chan);
    dma_channel_configure(
        data_chan,
        &data_dma,
        (void *)xram, // dst
        &dma_fifo[2], // src
        1,
        false);

    // DMA move raw received data into RAM
    dma_channel_config fifo_dma = dma_channel_get_default_config(fifo_chan);
    channel_config_set_high_priority(&fifo_dma, true);
    channel_config_set_dreq(&fifo_dma, pio_get_dreq(VGA_PIX_PIO, VGA_PIX_XRAM_SM, false));
    channel_config_set_read_increment(&fifo_dma, false);
    channel_config_set_chain_to(&fifo_dma, copy_chan);
    dma_channel_configure(
        fifo_chan,
        &fifo_dma,
        &dma_fifo[0],                       // dst
        &VGA_PIX_PIO->rxf[VGA_PIX_XRAM_SM], // src
        1,
        true);
}

void pix_task(void)
{
    while (!pio_sm_is_rx_fifo_empty(VGA_PIX_PIO, VGA_PIX_REGS_SM))
    {
        uint32_t raw = pio_sm_get(VGA_PIX_PIO, VGA_PIX_REGS_SM);
        uint8_t ch = (raw & 0x0F000000) >> 24;
        uint8_t addr = (raw & 0x00FF0000) >> 16;
        uint16_t word = raw & 0xFFFF;
        // These return true on slow operations to
        // allow us to stay greedy on fast ones.
        if (ch == 0 && pix_ch0_xreg(addr, word))
            break;
        if (ch == 15 && pix_ch15_xreg(addr, word))
            break;
    }
}
