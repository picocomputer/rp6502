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
#include <stdio.h>

// Rumbledethumps Interface Adapter for 6502/6800.

// 120MHz clk_sys can run a 6502 at 2MHz. Overclock 240MHz for 4MHz.
// Once we're off the breadbord, I think it's possible to
// synchronize address bus reads with PHI2 and get to 8MHz.

// Weird stuff I ran into while placing critical memory.
// I think the Pi Pico SDK needs fixing but I'm not entirely sure.
// The uninitialized_ram() macro is supposed to handle vram,
// but it doesn't match the linker script and only makes syntax errors.
// The scratch_x() macro is supposed to handle regs,
// but the program fails to launch for debug.
// Debugging a 64KB array locks up. Since this happens too
// easily with visual debuggers, the array is hacked up.

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
} vram_blocks
    __attribute__((aligned(0x10000)))
    __attribute__((section(".uninitialized_data.vram")));
uint8_t *const vram = (uint8_t *)&vram_blocks;
#endif

extern volatile uint8_t regs[0xFF];
asm(".equ regs, 0x20040000");

// Set clock for 6502 PHI2.
void ria_set_phi2_mhz(int mhz)
{
    // Only GP21 can be used for this.
    int div = 48 / mhz;
    assert(mhz <= 8);    // 8MHz max
    assert(!(48 % div)); // even divisions only
    clock_gpio_init(21, clk_sys, div);
}

void ria_init()
{
    // 120MHz clk_sys allows 1,2,3,4,5,6,8 MHz PHI2.
    set_sys_clock_khz(120 * 1000, true);

    // safety check for compiler alignment
    assert(!((uintptr_t)vram & 0xFFFF));
    assert(!((uintptr_t)regs & 0x1F));

    static const PIO pio = pio1;
    uint addr_data_sm = pio_claim_unused_sm(pio, true);

    // This pio program pulls in a 5 bit address bus (32 bytes FFE0-FFFF)
    // and shifts out an 8 bit data bus.
    uint addr_data_offset = pio_add_program(pio, &ria_addr_data_program);
    pio_sm_config addr_data_config = ria_addr_data_program_get_default_config(addr_data_offset);
    sm_config_set_in_pins(&addr_data_config, 2);
    sm_config_set_in_shift(&addr_data_config, false, true, 5);
    pio_sm_set_consecutive_pindirs(pio, addr_data_sm, 2, 5, false);
    sm_config_set_out_pins(&addr_data_config, 8, 8);
    sm_config_set_out_shift(&addr_data_config, true, true, 8);
    for (int i = 8; i < 16; i++)
        pio_gpio_init(pio, i);
    pio_sm_set_consecutive_pindirs(pio, addr_data_sm, 8, 8, true);
    pio_sm_init(pio, addr_data_sm, addr_data_offset, &addr_data_config);
    pio_sm_put(pio, addr_data_sm, (uintptr_t)regs >> 5);
    pio_sm_set_enabled(pio, addr_data_sm, true);

    int addr_chan = dma_claim_unused_channel(true);
    int data_chan = dma_claim_unused_channel(true);

    // The address from PIO is first programmed into the data DMA.
    dma_channel_config addr_dma = dma_channel_get_default_config(addr_chan);
    channel_config_set_read_increment(&addr_dma, false);
    channel_config_set_chain_to(&addr_dma, data_chan);
    dma_channel_configure(
        addr_chan,                        // Channel to be configured
        &addr_dma,                        // The configuration we just created
        &dma_hw->ch[data_chan].read_addr, // The initial write address
        &pio->rxf[addr_data_sm],          // The initial read address
        1,                                // Number of transfers; in this case each is 1 byte.
        false                             // Start immediately.
    );

    // Now move the requested memory data to PIO.
    dma_channel_config data_dma = dma_channel_get_default_config(data_chan);
    channel_config_set_transfer_data_size(&data_dma, DMA_SIZE_8);
    channel_config_set_chain_to(&data_dma, addr_chan);
    dma_channel_configure(
        data_chan,               // Channel to be configured
        &data_dma,               // The configuration we just created
        &pio->txf[addr_data_sm], // The initial write address
        regs,                    // The initial read address
        1,                       // Number of transfers; in this case each is 1 byte.
        false                    // Start immediately.
    );

    // Go!
    dma_channel_start(addr_chan);

    // Temporary memory data so I can measure timing on scope
    regs[0] = 0;
    regs[8] = 0xff;
}

void ria_task()
{
    // No CPU needed!
}
