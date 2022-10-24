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

// Rumbledethumps Interface Adapter for WDC W65C02S.
// Pi Pico sys clock of 120MHz will run 6502 at 4MHz.

#define RIA_PIO pio1
// Content of these 15 pins is bound to the PIO program structure.
#define RIA_PIN_BASE 0
#define RIA_CS_PIN (RIA_PIN_BASE + 0)
#define RIA_RWB_PIN (RIA_PIN_BASE + 1)
#define RIA_DATA_PIN_BASE (RIA_PIN_BASE + 2)
#define RIA_ADDR_PIN_BASE (RIA_PIN_BASE + 10)
// These pins may be freely moved around but PHI2 on 21 is strongly
// recommended since no other pins support clock_gpio_init().
#define RIA_A16_PIN 15
#define RIA_PHI2_PIN 21
#define RIA_RESB_PIN 22
#define RIA_IRQB_PIN 28
// Clock changes needs the UARTs retimed too, so we own this for now
#define RIA_UART uart0
#define RIA_UART_BAUD_RATE 115200
#define RIA_UART_TX_PIN 16
#define RIA_UART_RX_PIN 17

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

int ria_write_sm;
int ria_read_sm;
uint32_t ria_phi2_khz;
uint8_t ria_reset_ms;
bool ria_halted;
absolute_time_t ria_reset_timer;
enum state
{
    halt,
    reset,
    run
} ria_state;

static void ria_write_init()
{
    // PIO to manage reading and writing
    ria_write_sm = pio_claim_unused_sm(RIA_PIO, true);
    uint offset = pio_add_program(RIA_PIO, &ria_write_program);
    pio_sm_config config = ria_write_program_get_default_config(offset);
    sm_config_set_in_pins(&config, RIA_PIN_BASE);
    sm_config_set_in_shift(&config, false, false, 0);
    sm_config_set_out_pins(&config, RIA_DATA_PIN_BASE, 8);
    sm_config_set_sideset_pins(&config, RIA_PHI2_PIN);
    pio_gpio_init(RIA_PIO, RIA_PHI2_PIN);
    pio_sm_set_consecutive_pindirs(RIA_PIO, ria_write_sm, RIA_PHI2_PIN, 1, true);
    pio_sm_init(RIA_PIO, ria_write_sm, offset, &config);
    pio_sm_put(RIA_PIO, ria_write_sm, (uintptr_t)regs >> 5);
    pio_sm_exec_wait_blocking(RIA_PIO, ria_write_sm, pio_encode_pull(false, true));
    pio_sm_exec_wait_blocking(RIA_PIO, ria_write_sm, pio_encode_mov(pio_y, pio_osr));
    pio_sm_set_enabled(RIA_PIO, ria_write_sm, true);

    // Need both channels now to configure chain ping-pong
    int addr_chan = dma_claim_unused_channel(true);
    int data_chan = dma_claim_unused_channel(true);

    // DMA move the requested memory data to PIO for output
    dma_channel_config data_dma = dma_channel_get_default_config(data_chan);
    channel_config_set_high_priority(&data_dma, true);
    channel_config_set_dreq(&data_dma, pio_get_dreq(RIA_PIO, ria_write_sm, false));
    channel_config_set_read_increment(&data_dma, false);
    channel_config_set_transfer_data_size(&data_dma, DMA_SIZE_8);
    channel_config_set_chain_to(&data_dma, addr_chan);
    dma_channel_configure(
        data_chan,
        &data_dma,
        regs,                        // dst
        &RIA_PIO->rxf[ria_write_sm], // src
        1,
        false);

    // DMA move address from PIO into the data DMA config
    dma_channel_config addr_dma = dma_channel_get_default_config(addr_chan);
    channel_config_set_high_priority(&addr_dma, true);
    channel_config_set_dreq(&addr_dma, pio_get_dreq(RIA_PIO, ria_write_sm, false));
    channel_config_set_read_increment(&addr_dma, false);
    channel_config_set_chain_to(&addr_dma, data_chan);
    dma_channel_configure(
        addr_chan,
        &addr_dma,
        &dma_channel_hw_addr(data_chan)->write_addr, // dst
        &RIA_PIO->rxf[ria_write_sm],                 // src
        1,
        true);
}

static void ria_read_init()
{
    // PIO to pull in a 5 bit address bus and shift out an 8 bit data bus.
    ria_read_sm = pio_claim_unused_sm(RIA_PIO, true);
    uint offset = pio_add_program(RIA_PIO, &ria_read_program);
    pio_sm_config config = ria_read_program_get_default_config(offset);
    sm_config_set_in_pins(&config, RIA_ADDR_PIN_BASE);
    sm_config_set_in_shift(&config, false, true, 5);
    sm_config_set_out_pins(&config, RIA_DATA_PIN_BASE, 8);
    sm_config_set_out_shift(&config, true, true, 8);
    for (int i = RIA_DATA_PIN_BASE; i < RIA_DATA_PIN_BASE + 8; i++)
        pio_gpio_init(RIA_PIO, i);
    pio_sm_set_consecutive_pindirs(RIA_PIO, ria_read_sm, RIA_DATA_PIN_BASE, 8, true);
    pio_sm_init(RIA_PIO, ria_read_sm, offset, &config);
    pio_sm_put(RIA_PIO, ria_read_sm, (uintptr_t)regs >> 5);
    pio_sm_exec_wait_blocking(RIA_PIO, ria_read_sm, pio_encode_pull(false, true));
    pio_sm_exec_wait_blocking(RIA_PIO, ria_read_sm, pio_encode_mov(pio_y, pio_osr));
    pio_sm_set_enabled(RIA_PIO, ria_read_sm, true);

    // Need both channels now to configure chain ping-pong
    int addr_chan = dma_claim_unused_channel(true);
    int data_chan = dma_claim_unused_channel(true);

    // DMA move the requested memory data to PIO for output
    dma_channel_config data_dma = dma_channel_get_default_config(data_chan);
    channel_config_set_high_priority(&data_dma, true);
    channel_config_set_dreq(&data_dma, pio_get_dreq(RIA_PIO, ria_read_sm, true));
    channel_config_set_transfer_data_size(&data_dma, DMA_SIZE_8);
    channel_config_set_chain_to(&data_dma, addr_chan);
    dma_channel_configure(
        data_chan,
        &data_dma,
        &RIA_PIO->txf[ria_read_sm], // dst
        regs,                       // src
        1,
        false);

    // DMA move address from PIO into the data DMA config
    dma_channel_config addr_dma = dma_channel_get_default_config(addr_chan);
    channel_config_set_high_priority(&addr_dma, true);
    channel_config_set_dreq(&addr_dma, pio_get_dreq(RIA_PIO, ria_read_sm, false));
    channel_config_set_read_increment(&addr_dma, false);
    channel_config_set_chain_to(&addr_dma, data_chan);
    dma_channel_configure(
        addr_chan,
        &addr_dma,
        &dma_channel_hw_addr(data_chan)->read_addr, // dst
        &RIA_PIO->rxf[ria_read_sm],                 // src
        1,
        true);
}

static void ria_load_memcpy()
{
    // FFE0                  * = $FFE0
    // FFE0           START:
    // FFE0  A2 10           LDX #$10
    // FFE2           LOOP:
    // FFE2  BD E0 FF        LDA $FFDF,X
    // FFE5  9D 00 80        STA $7FFF,X
    // FFE8  CA              DEX
    // FFE9  D0 F7           BNE LOOP
    // FFEB           END:
    // FFEB  00              BRK
    // FFEC  E0 FF           .WORD START
    // FFEE  EB FF           .WORD END
}

static void ria_load_test_program_sram()
{
    for (int i = 0; i < 32; i++)
        regs[i] = 0xEA; // NOP
    // Reset Vector $FFE0
    regs[0x1C] = 0xE0;
    regs[0x1D] = 0xFF;
    // FFE0              * = $FFE0
    // FFE0  AE FF FF    LDX $FFFF
    // FFE3  E8          INX
    // FFE4  8E 00 80    STX $8000
    // FFE7  AC 00 80    LDY $8000
    // FFEA  8C FF FF    STY $FFFF
    // FFED  4C ED FF    JMP $FFED
    regs[0x00] = 0xAE;
    regs[0x01] = 0xFF;
    regs[0x02] = 0xFF;
    regs[0x03] = 0xE8;
    regs[0x04] = 0x8E;
    regs[0x05] = 0x00;
    regs[0x06] = 0x80;
    regs[0x07] = 0xAC;
    regs[0x08] = 0x00;
    regs[0x09] = 0x80;
    regs[0x0a] = 0x8C;
    regs[0x0b] = 0xFF;
    regs[0x0c] = 0xFF;
    regs[0x0d] = 0x4C;
    regs[0x0e] = 0xED;
    regs[0x0f] = 0xFF;
}

void ria_stdio_init()
{
    stdio_uart_init_full(RIA_UART, RIA_UART_BAUD_RATE, RIA_UART_TX_PIN, RIA_UART_RX_PIN);
}

void ria_init()
{
    // safety check for compiler alignment
    assert(!((uintptr_t)regs & 0x1F));
    assert(!((uintptr_t)vram & 0xFFFF));

    // Turn off GPIO decorators that can delay input
    for (int i = RIA_PIN_BASE; i < RIA_PIN_BASE + 15; i++)
    {
        gpio_set_input_hysteresis_enabled(i, false);
        hw_set_bits(&RIA_PIO->input_sync_bypass, 1u << i);
    }

    // Raise DMA above CPU on crossbar
    bus_ctrl_hw->priority |=
        BUSCTRL_BUS_PRIORITY_DMA_R_BITS |
        BUSCTRL_BUS_PRIORITY_DMA_W_BITS;

    // drive reset pin
    gpio_init(RIA_RESB_PIN);
    gpio_set_dir(RIA_RESB_PIN, true);

    // the inits
    ria_write_init();
    ria_read_init();
    ria_set_phi2_khz(4000);
    ria_set_reset_ms(0);
    ria_halt();

    // todo remove later
    ria_load_test_program_sram();
    ria_reset();
}

void ria_task()
{
    // Report unexpected FIFO overflows and underflows
    static uint32_t fdebug = 0;
    uint32_t masked_fdebug = RIA_PIO->fdebug;
    masked_fdebug &= 0x0F0F0F0F;                 // reserved
    masked_fdebug &= ~(1 << (24 + ria_read_sm)); // expected
    if (fdebug != masked_fdebug)
    {
        fdebug = masked_fdebug;
        printf("PIO fdebug: %lX\n", fdebug);
    }

    // Reset timer
    if (ria_state == reset)
    {
        absolute_time_t now = get_absolute_time();
        if (absolute_time_diff_us(now, ria_reset_timer) < 0)
        {
            ria_state = run;
            gpio_put(RIA_RESB_PIN, true);
        }
    }

    // debug code to show writes
    static uint8_t regff = 0xEA;
    if (regs[31] != regff)
    {
        printf("Write: $FFFF = $%02X\n", regs[31]);
        regff = regs[31];
    }
}

// Set the 6502 clock frequency. Returns false on failure.
bool ria_set_phi2_khz(uint32_t freq_khz)
{
    if (!freq_khz)
        return false;
    uint32_t sys_clk_khz = freq_khz * 30;
    uint16_t clkdiv_int = 1;
    uint8_t clkdiv_frac = 0;
    if (sys_clk_khz < 120 * 1000)
    {
        // <=4MHz will always succeed but may have minor quantization and judder.
        // <=4MHz resolution is limited by the divider's 8-bit fraction.
        sys_clk_khz = 120 * 1000;
        clkdiv_int = sys_clk_khz / 30 / freq_khz;
        clkdiv_frac = ((float)sys_clk_khz / 30 / freq_khz - clkdiv_int) * (1u << 8u);
    }
    // >4MHz will clock the Pi Pico past 120MHz and may fail but will not judder.
    // >4MHz resolution is 100kHz. e.g. 7.1MHz, 7.2MHz, 7.3MHz
    uint32_t old_sys_clk_hz = clock_get_hz(clk_sys);
    if (!set_sys_clock_khz(sys_clk_khz, false))
        return false;
    pio_sm_set_clkdiv_int_frac(RIA_PIO, ria_write_sm, clkdiv_int, clkdiv_frac);
    if (old_sys_clk_hz != clock_get_hz(clk_sys))
        ria_stdio_init();
    ria_phi2_khz = sys_clk_khz / 30 / (clkdiv_int + clkdiv_frac / 256.f);
    return true;
}

// Return actual 6502 frequency adjusted for divider quantization.
uint32_t ria_get_phi2_khz()
{
    return ria_phi2_khz;
}

// Specify a minimum time for reset low. 0=auto
void ria_set_reset_ms(uint8_t ms)
{
    ria_reset_ms = ms;
}

// Return actual reset time. May be higher than requested
// to guarantee the 6502 gets two clock cycles during reset.
uint8_t ria_get_reset_ms()
{
    uint8_t reset_ms = ria_reset_ms;
    if (ria_phi2_khz == 1 && reset_ms < 3)
        reset_ms = 3;
    if (ria_phi2_khz == 2 && reset_ms < 2)
        reset_ms = 2;
    if (!reset_ms)
        reset_ms = 1;
    return reset_ms;
}

void ria_halt()
{
    ria_state = halt;
    gpio_put(RIA_RESB_PIN, false);
    ria_reset_timer = delayed_by_us(get_absolute_time(),
                                    (uint64_t)1000 * ria_get_reset_ms());
}

void ria_reset()
{
    if (ria_state != halt)
        ria_halt();
    ria_state = reset;
}

void ria_reset_button()
{
    printf("Reset\n");
    ria_reset();
}

void ria_test_button()
{
}
