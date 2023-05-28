/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mon/mon.h"
#include "main.h"
#include "pix.h"
#include "ria.h"
#include "act.h"
#include "api.h"
#include "ria.pio.h"
#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/structs/bus_ctrl.h"
#include <stdio.h>

// RP6502 Interface Adapter for WDC W65C02S.

#define RIA_WRITE_PIO pio0
#define RIA_WRITE_SM 0
#define RIA_READ_PIO pio0
#define RIA_READ_SM 1

static void ria_write_pio_init()
{
    // PIO to manage PHI2 clock and 6502 writes
    uint offset = pio_add_program(RIA_WRITE_PIO, &ria_write_program);
    pio_sm_config config = ria_write_program_get_default_config(offset);
    sm_config_set_in_pins(&config, RIA_PIN_BASE);
    sm_config_set_in_shift(&config, false, false, 0);
    sm_config_set_out_pins(&config, RIA_DATA_PIN_BASE, 8);
    sm_config_set_sideset_pins(&config, RIA_PHI2_PIN);
    pio_gpio_init(RIA_WRITE_PIO, RIA_PHI2_PIN);
    pio_sm_set_consecutive_pindirs(RIA_WRITE_PIO, RIA_WRITE_SM, RIA_PHI2_PIN, 1, true);
    pio_sm_init(RIA_WRITE_PIO, RIA_WRITE_SM, offset, &config);
    pio_sm_put(RIA_WRITE_PIO, RIA_WRITE_SM, (uintptr_t)regs >> 5);
    pio_sm_exec_wait_blocking(RIA_WRITE_PIO, RIA_WRITE_SM, pio_encode_pull(false, true));
    pio_sm_exec_wait_blocking(RIA_WRITE_PIO, RIA_WRITE_SM, pio_encode_mov(pio_y, pio_osr));
    pio_sm_set_enabled(RIA_WRITE_PIO, RIA_WRITE_SM, true);

    // Need both channels now to configure chain ping-pong
    int addr_chan = dma_claim_unused_channel(true);
    int data_chan = dma_claim_unused_channel(true);

    // DMA move the requested memory data to PIO for output
    dma_channel_config data_dma = dma_channel_get_default_config(data_chan);
    channel_config_set_high_priority(&data_dma, true);
    channel_config_set_dreq(&data_dma, pio_get_dreq(RIA_WRITE_PIO, RIA_WRITE_SM, false));
    channel_config_set_read_increment(&data_dma, false);
    channel_config_set_transfer_data_size(&data_dma, DMA_SIZE_8);
    channel_config_set_chain_to(&data_dma, addr_chan);
    dma_channel_configure(
        data_chan,
        &data_dma,
        regs,                              // dst
        &RIA_WRITE_PIO->rxf[RIA_WRITE_SM], // src
        1,
        false);

    // DMA move address from PIO into the data DMA config
    dma_channel_config addr_dma = dma_channel_get_default_config(addr_chan);
    channel_config_set_high_priority(&addr_dma, true);
    channel_config_set_dreq(&addr_dma, pio_get_dreq(RIA_WRITE_PIO, RIA_WRITE_SM, false));
    channel_config_set_read_increment(&addr_dma, false);
    channel_config_set_chain_to(&addr_dma, data_chan);
    dma_channel_configure(
        addr_chan,
        &addr_dma,
        &dma_channel_hw_addr(data_chan)->write_addr, // dst
        &RIA_WRITE_PIO->rxf[RIA_WRITE_SM],           // src
        1,
        true);
}

static void ria_read_pio_init()
{
    // PIO for 6502 reads
    uint offset = pio_add_program(RIA_READ_PIO, &ria_read_program);
    pio_sm_config config = ria_read_program_get_default_config(offset);
    sm_config_set_in_pins(&config, RIA_ADDR_PIN_BASE);
    sm_config_set_in_shift(&config, false, true, 5);
    sm_config_set_out_pins(&config, RIA_DATA_PIN_BASE, 8);
    sm_config_set_out_shift(&config, true, true, 8);
    for (int i = RIA_DATA_PIN_BASE; i < RIA_DATA_PIN_BASE + 8; i++)
        pio_gpio_init(RIA_READ_PIO, i);
    pio_sm_set_consecutive_pindirs(RIA_READ_PIO, RIA_READ_SM, RIA_DATA_PIN_BASE, 8, true);
    pio_sm_init(RIA_READ_PIO, RIA_READ_SM, offset, &config);
    pio_sm_put(RIA_READ_PIO, RIA_READ_SM, (uintptr_t)regs >> 5);
    pio_sm_exec_wait_blocking(RIA_READ_PIO, RIA_READ_SM, pio_encode_pull(false, true));
    pio_sm_exec_wait_blocking(RIA_READ_PIO, RIA_READ_SM, pio_encode_mov(pio_y, pio_osr));
    pio_sm_set_enabled(RIA_READ_PIO, RIA_READ_SM, true);

    // Need both channels now to configure chain ping-pong
    int addr_chan = dma_claim_unused_channel(true);
    int data_chan = dma_claim_unused_channel(true);

    // DMA move the requested memory data to PIO for output
    dma_channel_config data_dma = dma_channel_get_default_config(data_chan);
    channel_config_set_high_priority(&data_dma, true);
    channel_config_set_dreq(&data_dma, pio_get_dreq(RIA_READ_PIO, RIA_READ_SM, true));
    channel_config_set_transfer_data_size(&data_dma, DMA_SIZE_8);
    channel_config_set_chain_to(&data_dma, addr_chan);
    dma_channel_configure(
        data_chan,
        &data_dma,
        &RIA_READ_PIO->txf[RIA_READ_SM], // dst
        regs,                            // src
        1,
        false);

    // DMA move address from PIO into the data DMA config
    dma_channel_config addr_dma = dma_channel_get_default_config(addr_chan);
    channel_config_set_high_priority(&addr_dma, true);
    channel_config_set_dreq(&addr_dma, pio_get_dreq(RIA_READ_PIO, RIA_READ_SM, false));
    channel_config_set_read_increment(&addr_dma, false);
    channel_config_set_chain_to(&addr_dma, data_chan);
    dma_channel_configure(
        addr_chan,
        &addr_dma,
        &dma_channel_hw_addr(data_chan)->read_addr, // dst
        &RIA_READ_PIO->rxf[RIA_READ_SM],            // src
        1,
        true);
}

void ria_init()
{
    // safety check for compiler alignment
    assert(!((uintptr_t)regs & 0x1F));

    // Adjustments for GPIO performance. Important!
    for (int i = RIA_PIN_BASE; i < RIA_PIN_BASE + 15; i++)
    {
        gpio_set_pulls(i, true, true);
        gpio_set_input_hysteresis_enabled(i, false);
        hw_set_bits(&pio0->input_sync_bypass, 1u << i);
        hw_set_bits(&pio1->input_sync_bypass, 1u << i);
    }

    // Raise DMA above CPU on crossbar
    bus_ctrl_hw->priority |=
        BUSCTRL_BUS_PRIORITY_DMA_R_BITS |
        BUSCTRL_BUS_PRIORITY_DMA_W_BITS;

    // the inits
    ria_write_pio_init();
    ria_read_pio_init();
}

void ria_task()
{
    // Report unexpected FIFO overflows and underflows
    // TODO needs much improvement
    uint32_t fdebug = pio0->fdebug;
    uint32_t masked_fdebug = fdebug & 0x0F0F0F0F; // reserved
    masked_fdebug &= ~(1 << (24 + RIA_READ_SM));  // expected
    if (masked_fdebug)
    {
        pio0->fdebug = 0xFF;
        printf("pio0->fdebug: %lX\n", fdebug);
    }
}

void ria_reclock(uint16_t clkdiv_int, uint8_t clkdiv_frac)
{
    pio_sm_set_clkdiv_int_frac(RIA_WRITE_PIO, RIA_WRITE_SM, clkdiv_int, clkdiv_frac);
    pio_sm_set_clkdiv_int_frac(RIA_READ_PIO, RIA_READ_SM, clkdiv_int, clkdiv_frac);
}
