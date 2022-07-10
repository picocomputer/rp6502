/*
 * Copyright (c) 2022 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ria.h"
#include "ria.pio.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/structs/bus_ctrl.h"
#include <stdio.h>

// Rumbledethumps Interface Adapter for WDC 6502.
// Pi Pico sys clock of 120MHz can run a 6502 at 4MHz.

#define RIA_PIO pio1
#define RIA_ADDR_PIN_BASE 2
#define RIA_DATA_PIN_BASE 8
#define RIA_PHI2_PIN 21
#define RIA_RESB_PIN 22

extern volatile uint8_t regs[0x1F];
asm(".equ regs, 0x20040000");

#ifdef NDEBUG
uint8_t vram[0xFFFF]
    __attribute__((aligned(0x10000)))
    __attribute__((section(".uninitialized_data.vram")));
#else
struct Vram
{
    uint8_t _0[0xFFF];
    uint8_t _1[0xFFF];
    uint8_t _2[0xFFF];
    uint8_t _3[0xFFF];
    uint8_t _4[0xFFF];
    uint8_t _5[0xFFF];
    uint8_t _6[0xFFF];
    uint8_t _7[0xFFF];
    uint8_t _8[0xFFF];
    uint8_t _9[0xFFF];
    uint8_t _A[0xFFF];
    uint8_t _B[0xFFF];
    uint8_t _C[0xFFF];
    uint8_t _D[0xFFF];
    uint8_t _E[0xFFF];
    uint8_t _F[0xFFF];
    // this struct of 4KB segments is because
    // a single 64KB array crashes my debugger
} vram_blocks
    __attribute__((aligned(0x10000)))
    __attribute__((section(".uninitialized_data.vram")));
uint8_t *const vram = (uint8_t *)&vram_blocks;
#endif

// address watcher for debug
uint addr_watch_sm;

void ria_init()
{
    // safety check for compiler alignment
    assert(!((uintptr_t)regs & 0x1F));
    assert(!((uintptr_t)vram & 0xFFFF));

    // 120MHz clk_sys allows 1,2,3,4,5,6,8 MHz PHI2.
    set_sys_clock_khz(120 * 1000, true);

    // Begin reset
    gpio_init(RIA_RESB_PIN);
    gpio_set_dir(RIA_RESB_PIN, true);

#ifndef true
    // Manual PHI2
    gpio_init(RIA_PHI2_PIN);
    gpio_set_dir(RIA_PHI2_PIN, true);
#else
    // Auto PHI2
    ria_set_phi2_khz(4 * 1000);
#endif

    // Turn off GPIO decorators that can delay address input
    for (int i = RIA_ADDR_PIN_BASE; i < RIA_ADDR_PIN_BASE + 5; i++) {
        gpio_set_input_hysteresis_enabled(i, false);
        hw_set_bits(&RIA_PIO->input_sync_bypass, 1u << i);
    }

    // PIO to pull in a 5 bit address bus and shift out an 8 bit data bus.
    uint addr_data_sm = pio_claim_unused_sm(RIA_PIO, true);
    uint addr_data_offset = pio_add_program(RIA_PIO, &ria_addr_data_program);
    pio_sm_config addr_data_config = ria_addr_data_program_get_default_config(addr_data_offset);
    sm_config_set_in_pins(&addr_data_config, RIA_ADDR_PIN_BASE);
    sm_config_set_in_shift(&addr_data_config, false, true, 5);
    sm_config_set_out_pins(&addr_data_config, RIA_DATA_PIN_BASE, 8);
    sm_config_set_out_shift(&addr_data_config, true, true, 8);
    for (int i = RIA_DATA_PIN_BASE; i < RIA_DATA_PIN_BASE + 8; i++)
        pio_gpio_init(RIA_PIO, i);
    pio_sm_set_consecutive_pindirs(RIA_PIO, addr_data_sm, RIA_DATA_PIN_BASE, 8, true);
    pio_sm_init(RIA_PIO, addr_data_sm, addr_data_offset, &addr_data_config);
    pio_sm_put(RIA_PIO, addr_data_sm, (uintptr_t)regs >> 5);
    pio_sm_set_enabled(RIA_PIO, addr_data_sm, true);

    // PIO to watch changes on the address bus for debug in manual mode
    addr_watch_sm = pio_claim_unused_sm(RIA_PIO, true);
    uint addr_watch_offset = pio_add_program(RIA_PIO, &ria_addr_watch_program);
    pio_sm_config addr_watch_config = ria_addr_watch_program_get_default_config(addr_watch_offset);
    sm_config_set_in_pins(&addr_watch_config, RIA_ADDR_PIN_BASE);
    sm_config_set_in_shift(&addr_watch_config, false, false, 5);
    pio_sm_init(RIA_PIO, addr_watch_sm, addr_watch_offset, &addr_watch_config);
    pio_sm_set_enabled(RIA_PIO, addr_watch_sm, true);
    pio_sm_get_blocking(RIA_PIO, addr_watch_sm); // Eat first report

    // Raise DMA above CPU on crossbar
    bus_ctrl_hw->priority |=
        BUSCTRL_BUS_PRIORITY_DMA_R_BITS |
        BUSCTRL_BUS_PRIORITY_DMA_W_BITS;

    // Need both channels now to configure chain ping-pong
    int addr_chan = dma_claim_unused_channel(true);
    int data_chan = dma_claim_unused_channel(true);

    // DMA move the requested memory data to PIO for output
    dma_channel_config data_dma = dma_channel_get_default_config(data_chan);
    channel_config_set_high_priority(&data_dma, true);
    channel_config_set_dreq(&data_dma, pio_get_dreq(RIA_PIO, addr_data_sm, true));
    channel_config_set_transfer_data_size(&data_dma, DMA_SIZE_8);
    channel_config_set_chain_to(&data_dma, addr_chan);
    dma_channel_configure(
        data_chan,
        &data_dma,
        &RIA_PIO->txf[addr_data_sm],
        regs,
        1,
        false);

    // DMA move address from PIO into the data DMA config
    dma_channel_config addr_dma = dma_channel_get_default_config(addr_chan);
    channel_config_set_high_priority(&addr_dma, true);
    channel_config_set_dreq(&addr_dma, pio_get_dreq(RIA_PIO, addr_data_sm, false));
    channel_config_set_read_increment(&addr_dma, false);
    channel_config_set_chain_to(&addr_dma, data_chan);
    dma_channel_configure(
        addr_chan,
        &addr_dma,
        &dma_channel_hw_addr(data_chan)->read_addr,
        &RIA_PIO->rxf[addr_data_sm],
        1,
        true);

    // Temporary memory data
    for (int i = 0; i < 32; i++)
        regs[i] = 0xEA; //       EA        NOP
    regs[0x1B] = 0x4C;  // FFFB  4C FB FF  JMP $FFFB
    regs[0x1C] = 0xFB;
    regs[0x1D] = 0xFF;

    // Exit reset unless clock is manual
    if (gpio_get_function(RIA_PHI2_PIN) == GPIO_FUNC_GPCK)
    {
        gpio_put(RIA_RESB_PIN, true);
    }
}

void ria_task()
{
    // Print address changes when clock is manual
    if (gpio_get_function(RIA_PHI2_PIN) != GPIO_FUNC_GPCK &&
        !pio_sm_is_rx_fifo_empty(RIA_PIO, addr_watch_sm))
    {
        uint32_t addr = pio_sm_get(RIA_PIO, addr_watch_sm);
        printf("Addr: 0x%X\n", addr | 0xFFE0);
    }
}

// Set clock for 6502 PHI2.
void ria_set_phi2_khz(uint32_t khz)
{
    assert(khz <= 8000); // 8MHz max
    uint32_t clk = clock_get_hz(clk_sys);
    uint32_t div = clk / (khz * 1000);
    assert(!(clk % div)); // even divisions only
    clock_gpio_init(RIA_PHI2_PIN, CLOCKS_CLK_GPOUT0_CTRL_AUXSRC_VALUE_CLK_SYS, div);
}

void ria_reset_button()
{
    printf("Reset\n");
    gpio_put(RIA_RESB_PIN, false);
    sleep_ms(1);
    gpio_put(RIA_PHI2_PIN, true);
    sleep_ms(1);
    gpio_put(RIA_PHI2_PIN, false);
    sleep_ms(1);
    gpio_put(RIA_PHI2_PIN, true);
    sleep_ms(1);
    gpio_put(RIA_PHI2_PIN, false);
    sleep_ms(1);
    gpio_put(RIA_RESB_PIN, true);
}

void ria_clock_button()
{
    gpio_put(RIA_PHI2_PIN, true);
    sleep_ms(1);
    gpio_put(RIA_PHI2_PIN, false);
}
