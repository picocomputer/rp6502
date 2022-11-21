/*
 * Copyright (c) 2022 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mon.h"
#include "ria.h"
#include "ria.pio.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/structs/bus_ctrl.h"
#include <stdio.h>

// Rumbledethumps Interface Adapter for WDC W65C02S.
// Pi Pico sys clock of 120MHz will run 6502 at 4MHz.

// Content of these 15 pins is bound to the PIO program structure.
#define RIA_PIN_BASE 6
#define RIA_CS_PIN (RIA_PIN_BASE + 0)
#define RIA_RWB_PIN (RIA_PIN_BASE + 1)
#define RIA_DATA_PIN_BASE (RIA_PIN_BASE + 2)
#define RIA_ADDR_PIN_BASE (RIA_PIN_BASE + 10)
// These pins may be freely moved around but PHI2 on 21 is strongly
// recommended since no other pins support clock_gpio_init().
#define RIA_PHI2_PIN 21
#define RIA_RESB_PIN 28
#define RIA_IRQB_PIN 22
// Clock changes needs the UARTs retimed too, so we own this for now
#define RIA_UART uart1
#define RIA_UART_BAUD_RATE 115200
#define RIA_UART_TX_PIN 4
#define RIA_UART_RX_PIN 5
// Use both PIO blocks, constrained by address space
#define RIA_ACTION_PIO pio0
#define RIA_ACTION_SM 0
#define RIA_WRITE_PIO pio1
#define RIA_WRITE_SM 0
#define RIA_READ_PIO pio1
#define RIA_READ_SM 1

extern uint8_t regs[0x20];
#define REGS(addr) regs[addr & 0x1F]
#define REGSW(addr) ((uint16_t *)&REGS(addr))[0]

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

static uint32_t ria_phi2_khz;
static uint8_t ria_reset_ms;
static uint8_t ria_caps;
static absolute_time_t ria_reset_timer;
static volatile enum state {
    halt,
    reset,
    run,
    done
} ria_state;
static uint16_t ria_reset_vec = 0;
static volatile bool rw_in_progress = false;
static uint8_t *rw_buf = 0;
static size_t rw_pos;
static size_t rw_end;
static volatile int ria_inchar;

static void ria_action_loop();

// RIA action has one variable read address.
// 0 to disable (0 is hardcoded, disables by duplication).
static void ria_action_set_address(uint32_t addr)
{
    pio_sm_put(RIA_ACTION_PIO, RIA_ACTION_SM, addr & 0x1F);
}

// Stop the 6502
void ria_halt()
{
    gpio_put(RIA_RESB_PIN, false);
    rw_in_progress = false;
    ria_state = halt;
    ria_inchar = -1;
    REGS(0xFFE0) = 0;
    REGSW(0xFFFC) = ria_reset_vec;
    ria_action_set_address(0xFFE2);
    ria_reset_timer = delayed_by_us(get_absolute_time(),
                                    ria_get_reset_us());
}

// Start or reset the 6502
void ria_reset()
{
    if (ria_state != halt)
        ria_halt();
    ria_inchar = -1;
    REGS(0xFFE0) = 0;
    ria_state = reset;
}

static void ria_write_init()
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

static void ria_read_init()
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

static void ria_action_init()
{
    // PIO to supply action loop with events
    uint offset = pio_add_program(RIA_ACTION_PIO, &ria_action_program);
    pio_sm_config config = ria_action_program_get_default_config(offset);
    sm_config_set_in_pins(&config, RIA_PIN_BASE);
    sm_config_set_in_shift(&config, false, false, 0);
    pio_sm_init(RIA_ACTION_PIO, RIA_ACTION_SM, offset, &config);
    pio_sm_set_enabled(RIA_ACTION_PIO, RIA_ACTION_SM, true);
    ria_action_set_address(0);
}

void ria_stdio_init()
{
    stdio_uart_init_full(RIA_UART, RIA_UART_BAUD_RATE, RIA_UART_TX_PIN, RIA_UART_RX_PIN);
}

void ria_stdio_flush()
{
    while (getchar_timeout_us(0) >= 0)
        tight_loop_contents();
    while (!(uart_get_hw(RIA_UART)->fr & UART_UARTFR_TXFE_BITS))
        tight_loop_contents();
}

bool ria_is_active()
{
    return (ria_state == reset) || (ria_state == run);
}

void ria_init()
{
    // safety check for compiler alignment
    assert(!((uintptr_t)regs & 0x1F));
    assert(!((uintptr_t)vram & 0xFFFF));

    // Adjustments for GPIO. Speculating possible future needs.
    for (int i = RIA_PIN_BASE; i < RIA_PIN_BASE + 15; i++)
    {
        gpio_set_input_hysteresis_enabled(i, false);
        hw_set_bits(&pio0->input_sync_bypass, 1u << i);
        hw_set_bits(&pio1->input_sync_bypass, 1u << i);
        // gpio_set_drive_strength(i, GPIO_DRIVE_STRENGTH_12MA);
        // gpio_set_slew_rate(i, GPIO_SLEW_RATE_SLOW);
    }
    // gpio_set_input_hysteresis_enabled(RIA_PHI2_PIN, false);
    // hw_set_bits(&pio0->input_sync_bypass, 1u << RIA_PHI2_PIN);
    // hw_set_bits(&pio1->input_sync_bypass, 1u << RIA_PHI2_PIN);
    // gpio_set_drive_strength(RIA_PHI2_PIN, GPIO_DRIVE_STRENGTH_12MA);
    // gpio_set_slew_rate(RIA_PHI2_PIN, GPIO_SLEW_RATE_FAST);

    // Raise DMA above CPU on crossbar
    bus_ctrl_hw->priority |=
        BUSCTRL_BUS_PRIORITY_DMA_R_BITS |
        BUSCTRL_BUS_PRIORITY_DMA_W_BITS;

    // drive reset pin
    gpio_init(RIA_RESB_PIN);
    gpio_put(RIA_IRQB_PIN, false);
    gpio_set_dir(RIA_RESB_PIN, true);

    // drive irq pin
    gpio_init(RIA_IRQB_PIN);
    gpio_put(RIA_IRQB_PIN, true);
    gpio_set_dir(RIA_IRQB_PIN, true);

    // the inits
    ria_write_init();
    ria_read_init();
    ria_action_init();
    ria_set_phi2_khz(4000);
    ria_set_reset_ms(0);
    ria_set_caps(0);
    ria_halt();
    multicore_launch_core1(ria_action_loop);
}

void ria_task()
{
    // Report unexpected FIFO overflows and underflows
    uint32_t fdebug = pio0->fdebug;
    uint32_t masked_fdebug = fdebug & 0x0F0F0F0F;  // reserved
    masked_fdebug &= ~(1 << (24 + RIA_ACTION_SM)); // expected
    if (masked_fdebug)
    {
        pio0->fdebug = 0xFF;
        printf("pio0->fdebug: %lX\n", fdebug);
    }
    fdebug = pio1->fdebug;
    masked_fdebug = fdebug & 0x0F0F0F0F;         // reserved
    masked_fdebug &= ~(1 << (24 + RIA_READ_SM)); // expected
    if (masked_fdebug)
    {
        pio1->fdebug = 0xFF;
        printf("pio1->fdebug: %lX\n", fdebug);
    }

    // Reset 6502 when UART break signal received
    static uint32_t break_detect = 0;
    uint32_t current_break = uart_get_hw(RIA_UART)->rsr & UART_UARTRSR_BE_BITS;
    if (current_break)
    {
        hw_clear_bits(&uart_get_hw(RIA_UART)->rsr, UART_UARTRSR_BITS);
        if (ria_state != halt)
            ria_halt();
    }
    else if (break_detect)
        mon_halt();
    break_detect = current_break;

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

    if (ria_state == done)
    {
        ria_halt();
    }

    // Too expensive for action loop
    if (!rw_in_progress && ria_is_active() && ria_inchar < 0)
    {
        int ch = getchar_timeout_us(0);
        switch (ria_get_caps())
        {
        case 1:
            if (ch >= 'A' && ch <= 'Z')
            {
                ch += 32;
                break;
            }
            // fall through
        case 2:
            if (ch >= 'a' && ch <= 'z')
                ch -= 32;
        }
        ria_inchar = ch;
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
    // TODO make this never fail (round up, 50k to 6650, 100k to 8000, test by 5s?)
    uint32_t old_sys_clk_hz = clock_get_hz(clk_sys);
    ria_stdio_flush();
    if (!set_sys_clock_khz(sys_clk_khz, false))
        return false;
    pio_sm_set_clkdiv_int_frac(RIA_ACTION_PIO, RIA_ACTION_SM, clkdiv_int, clkdiv_frac);
    pio_sm_set_clkdiv_int_frac(RIA_WRITE_PIO, RIA_WRITE_SM, clkdiv_int, clkdiv_frac);
    pio_sm_set_clkdiv_int_frac(RIA_READ_PIO, RIA_READ_SM, clkdiv_int, clkdiv_frac);
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

uint8_t ria_get_reset_ms()
{
    return ria_reset_ms;
}

// Return calculated reset time. May be higher than requested
// to guarantee the 6502 gets two clock cycles during reset.
uint32_t ria_get_reset_us()
{
    if (!ria_reset_ms)
        return (2000001 / ria_get_phi2_khz() + 999) / 1000;
    if (ria_phi2_khz == 1 && ria_reset_ms == 1)
        return 2000;
    return ria_reset_ms * 1000;
}

void ria_set_caps(uint8_t mode)
{
    ria_caps = mode;
}

uint8_t ria_get_caps()
{
    return ria_caps;
}

void ria_ram_write(uint32_t addr, uint8_t *buf, size_t len)
{
    ria_halt();
    // forbidden area
    while (len && (addr + len > 0xFFF0))
        if (addr + --len <= 0xFFFF)
            REGS(addr + len) = buf[len];
    while (len && (addr + len > 0xFF00))
        len--;
    ria_reset_vec = REGSW(0xFFFC);
    if (!len)
        return;
    // Reset vector
    REGS(0xFFFC) = 0xF0;
    REGS(0xFFFD) = 0xFF;
    // Self-modifying fast load
    // FFF0  A9 00     LDA #$00
    // FFF2  8D 00 00  STA $0000
    // FFF5  80 F9     BRA $FFF0
    // FFF7  EA        NOP
    // FFF8  80 FE     BRA $FFF8
    REGS(0xFFF0) = 0xA9;
    REGS(0xFFF1) = buf[0];
    REGS(0xFFF2) = 0x8D;
    REGS(0xFFF3) = addr & 0xFF;
    REGS(0xFFF4) = addr >> 8;
    REGS(0xFFF5) = 0x80;
    REGS(0xFFF6) = 0xF9;
    REGS(0xFFF7) = 0xEA;
    REGS(0xFFF8) = 0x80;
    REGS(0xFFF9) = 0xFE;
    ria_action_set_address(0xFFF6);
    rw_in_progress = true;
    rw_buf = buf;
    rw_end = len;
    rw_pos = 0;
    if (rw_pos == rw_end)
        ria_state = done;
    else
    {
        if (++rw_pos == rw_end)
            REGS(0xFFF6) = 0x00;
        ria_reset();
    }
}

static inline void ria_action_ram_write()
{
    // action for case 0x16:
    if (rw_pos < rw_end)
    {
        REGS(0xFFF1) = rw_buf[rw_pos];
        REGSW(0xFFF3) += 1;
        if (++rw_pos == rw_end)
            REGS(0xFFF6) = 0x00;
    }
    else
        ria_state = done;
}

void ria_ram_read(uint32_t addr, uint8_t *buf, size_t len)
{
    ria_halt();
    // forbidden area
    while (len && (addr + len > 0xFFF0))
        if (addr + --len <= 0xFFFF)
            buf[len] = REGS(addr + len);
    if (!len)
        return;
    // Reset vector
    ria_reset_vec = REGSW(0xFFFC);
    REGS(0xFFFC) = 0xF0;
    REGS(0xFFFD) = 0xFF;
    // Self-modifying fast load
    // FFF0  AD 00 00  LDA $0000
    // FFF3  8D F8 FF  STA $FFFC
    // FFF6  80 F8     BRA $FFF0
    // FFF8  80 FE     BRA $FFF8
    REGS(0xFFF0) = 0xAD;
    REGS(0xFFF1) = addr & 0xFF;
    REGS(0xFFF2) = addr >> 8;
    REGS(0xFFF3) = 0x8D;
    REGS(0xFFF4) = 0xFC;
    REGS(0xFFF5) = 0xFF;
    REGS(0xFFF6) = 0x80;
    REGS(0xFFF7) = 0xF8;
    REGS(0xFFF8) = 0x80;
    REGS(0xFFF9) = 0xFE;
    ria_action_set_address(0xFFF7);
    rw_in_progress = true;
    rw_buf = buf;
    rw_end = len;
    rw_pos = 0;
    if (rw_pos + 1 == rw_end)
        REGS(0xFFF7) = 0x00;
    if (rw_pos == rw_end)
        ria_state = done;
    else
        ria_reset();
}

static inline void ria_action_ram_read()
{
    // action for case 0x17:
    if (rw_pos < rw_end)
    {
        REGSW(0xFFF1) += 1;
        rw_buf[rw_pos] = REGS(0xFFFC);
        if (++rw_pos == rw_end)
            ria_state = done;
        if (rw_pos + 1 == rw_end)
            REGS(0xFFF7) = 0x00;
    }
}

void ria_jmp(uint32_t addr)
{
    ria_halt();
    // Reset vector
    ria_reset_vec = REGSW(0xFFFC);
    REGS(0xFFFC) = 0xF0;
    REGS(0xFFFD) = 0xFF;
    // RESB doesn't clear these
    // FFF0  D8        CLD      ; clear decimal mode
    // FFF1  A2 FF     LDX #$FF ; top of stack
    // FFF3  9A        TXS      ; set the stack
    // FFF4  4C 00 00  JMP $0000
    REGS(0xFFF0) = 0xD8;
    REGS(0xFFF1) = 0xA2;
    REGS(0xFFF2) = 0xFF;
    REGS(0xFFF3) = 0x9A;
    REGS(0xFFF4) = 0x4C;
    REGS(0xFFF5) = addr & 0xFF;
    REGS(0xFFF6) = addr >> 8;
    ria_reset();
}

static void ria_action_loop()
{
    // In here we bypass the usual SDK calls as needed for performance.
    while (true)
    {
        if (!pio_sm_is_rx_fifo_empty(RIA_ACTION_PIO, RIA_ACTION_SM))
        {
            uint32_t addr = RIA_ACTION_PIO->rxf[RIA_ACTION_SM];
            uint32_t data = addr & 0xFF;
            addr = (addr >> 8) & 0x1F;
            if (gpio_get(RIA_RESB_PIN))
            {
                switch (addr)
                {
                case 0x16:
                    ria_action_ram_write();
                    break;
                case 0x17:
                    ria_action_ram_read();
                    break;
                case 0x0F:
                    ria_halt();
                    break;
                case 0x02:
                    if (ria_inchar >= 0)
                    {
                        REGS(0xFFE0) |= 0b01000000;
                        REGS(0xFFE2) = ria_inchar;
                        ria_inchar = -1;
                    }
                    else
                    {
                        REGS(0xFFE0) &= ~0b01000000;
                        REGS(0xFFE2) = 0;
                    }
                    break;
                case 0x01:
                    uart_get_hw(RIA_UART)->dr = data;
                    if (uart_is_writable(RIA_UART))
                        REGS(0xFFE0) |= 0b10000000;
                    else
                        REGS(0xFFE0) &= ~0b10000000;
                    break;
                case 0x00:
                    if (uart_is_writable(RIA_UART))
                        REGS(0xFFE0) |= 0b10000000;
                    else
                        REGS(0xFFE0) &= ~0b10000000;
                    if (!(REGS(0xFFE0) & 0b01000000) && ria_inchar >= 0)
                    {
                        REGS(0xFFE0) |= 0b01000000;
                        REGS(0xFFE2) = ria_inchar;
                        ria_inchar = -1;
                    }
                    break;
                }
            }
        }
    }
}

// Keep at end. Confuses code analyzer.
asm(".equ regs, 0x20040000");
