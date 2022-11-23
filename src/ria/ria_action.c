/*
 * Copyright (c) 2022 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ria.h"
#include "ria_action.h"
#include "ria_uart.h"
#include "ria.pio.h"
#include "regs.h"
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include <stdio.h>

static volatile bool rw_in_progress = false;
static uint16_t saved_reset_vec = 0;
static volatile uint8_t *volatile rw_buf = 0;
static volatile size_t rw_pos;
static volatile size_t rw_end;

// RIA action has one variable read address.
// 0 to disable (0 is hardcoded, disables by duplication).
static void ria_action_set_address(uint32_t addr)
{
    pio_sm_put(RIA_ACTION_PIO, RIA_ACTION_SM, addr & 0x1F);
}

void ria_action_reset()
{
    if (rw_in_progress)
    {
        rw_in_progress = false;
        REGSW(0xFFFC) = saved_reset_vec;
    }
    ria_action_set_address(0xFFE2);
}

bool ria_action_in_progress()
{
    return rw_in_progress;
}

void ria_action_pio_init()
{
    // PIO to supply action loop with events
    uint offset = pio_add_program(RIA_ACTION_PIO, &ria_action_program);
    pio_sm_config config = ria_action_program_get_default_config(offset);
    sm_config_set_in_pins(&config, RIA_PIN_BASE);
    sm_config_set_in_shift(&config, false, false, 0);
    pio_sm_init(RIA_ACTION_PIO, RIA_ACTION_SM, offset, &config);
    // Saving reset vector here preserves it after Pi Pico reset.
    saved_reset_vec = REGSW(0xFFFC);
    ria_action_reset();
    pio_sm_set_enabled(RIA_ACTION_PIO, RIA_ACTION_SM, true);
}

void ria_action_task()
{
    // Report unexpected FIFO overflows and underflows
    // TODO needs much improvement
    uint32_t fdebug = RIA_ACTION_PIO->fdebug;
    uint32_t masked_fdebug = fdebug & 0x0F0F0F0F;  // reserved
    masked_fdebug &= ~(1 << (24 + RIA_ACTION_SM)); // expected
    if (masked_fdebug)
    {
        RIA_ACTION_PIO->fdebug = 0xFF;
        printf("RIA_ACTION_PIO->fdebug: %lX\n", fdebug);
    }
}

void ria_action_ram_write(uint32_t addr, uint8_t *buf, size_t len)
{
    // avoid forbidden area
    while (len && (addr + len > 0xFFF0))
        if (addr + --len <= 0xFFFF)
            REGS(addr + len) = buf[len];
    while (len && (addr + len > 0xFF00))
        len--;
    saved_reset_vec = REGSW(0xFFFC);
    if (!len)
        return;
    ria_stop();
    // Reset vector
    REGSW(0xFFFC) = 0xFFF0;
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
    if (++rw_pos == rw_end)
        REGS(0xFFF6) = 0x00;
    ria_reset();
}

static inline void __force_inline ram_write()
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
        ria_done();
}

void ria_action_ram_read(uint32_t addr, uint8_t *buf, size_t len)
{
    // avoid forbidden area
    while (len && (addr + len > 0xFFF0))
        if (addr + --len <= 0xFFFF)
            buf[len] = REGS(addr + len);
    if (!len)
        return;
    ria_stop();
    // Reset vector
    saved_reset_vec = REGSW(0xFFFC);
    REGSW(0xFFFC) = 0xFFF0;
    // Self-modifying fast load
    // FFF0  AD 00 00  LDA $0000
    // FFF3  8D FC FF  STA $FFFC
    // FFF6  80 F8     BRA $FFF0
    REGS(0xFFF0) = 0xAD;
    REGS(0xFFF1) = addr & 0xFF;
    REGS(0xFFF2) = addr >> 8;
    REGS(0xFFF3) = 0x8D;
    REGS(0xFFF4) = 0xFC;
    REGS(0xFFF5) = 0xFF;
    REGS(0xFFF6) = 0x80;
    REGS(0xFFF7) = 0xF8;
    rw_in_progress = true;
    rw_buf = buf;
    rw_end = len;
    rw_pos = 0;
    ria_reset();
}

static inline void __force_inline ram_read(uint32_t data)
{
    if (rw_pos < rw_end)
    {
        REGSW(0xFFF1) += 1;
        rw_buf[rw_pos] = data;
        if (++rw_pos == rw_end)
            ria_done();
    }
}

void __no_inline_not_in_flash_func(ria_action_loop)()
{
    // In here we bypass the usual SDK calls as needed for performance.
    while (true)
    {
        if (!(RIA_ACTION_PIO->fstat & (1u << (PIO_FSTAT_RXEMPTY_LSB + RIA_ACTION_SM))))
        {
            uint32_t addr = RIA_ACTION_PIO->rxf[RIA_ACTION_SM];
            uint32_t data = addr & 0xFF;
            addr = (addr >> 8) & 0x1F;
            if (gpio_get(RIA_RESB_PIN))
            {
                if (rw_in_progress)
                    switch (addr)
                    {
                    case 0x16:
                        ram_write();
                        break;
                    case 0x1C:
                        ram_read(data);
                        break;
                    }
                else
                    switch (addr)
                    {
                    case 0x0F:
                        ria_done();
                        break;
                    case 0x02:
                        if (ria_uart_rx_char >= 0)
                        {
                            REGS(0xFFE0) |= 0b01000000;
                            REGS(0xFFE2) = ria_uart_rx_char;
                            ria_uart_rx_char = -1;
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
                        if (!(REGS(0xFFE0) & 0b01000000) && ria_uart_rx_char >= 0)
                        {
                            REGS(0xFFE0) |= 0b01000000;
                            REGS(0xFFE2) = ria_uart_rx_char;
                            ria_uart_rx_char = -1;
                        }
                        break;
                    }
            }
        }
    }
}
