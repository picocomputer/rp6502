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

// This is the smallest value that will
// allow 16-byte read/write operations at 1 kHz.
#define RIA_ACTION_WATCHDOG_MS 200

static volatile enum state {
    action_state_run = 0,
    action_state_read,
    action_state_write,
    action_state_verify
} volatile action_state = action_state_run;
static absolute_time_t action_watchdog_timer;
static volatile int32_t action_result = -1;
static int32_t saved_reset_vec = -1;
static volatile uint8_t *read_buf = 0;
static volatile const uint8_t *write_buf = 0;
static volatile int32_t rw_pos;
static volatile int32_t rw_end;

// RIA action has one variable read address.
static void ria_action_set_address(uint32_t addr)
{
    pio_sm_put(RIA_ACTION_PIO, RIA_ACTION_SM, addr & 0x1F);
}

// -1 good, -2 timeout, >=0 failed verify at address
int32_t ria_action_result()
{
    return action_result;
}

void ria_action_reset()
{
    action_state = action_state_run;
    ria_action_set_address(0xFFE2);
    if (saved_reset_vec > 0)
    {
        REGSW(0xFFFC) = saved_reset_vec;
        saved_reset_vec = -1;
    }
    action_watchdog_timer = delayed_by_us(get_absolute_time(),
                                          ria_get_reset_us() +
                                              RIA_ACTION_WATCHDOG_MS * 1000);
}

bool ria_action_in_progress()
{
    return action_state != action_state_run;
}

void ria_action_pio_init()
{
    // PIO to supply action loop with events
    uint offset = pio_add_program(RIA_ACTION_PIO, &ria_action_program);
    pio_sm_config config = ria_action_program_get_default_config(offset);
    sm_config_set_in_pins(&config, RIA_PIN_BASE);
    sm_config_set_in_shift(&config, false, false, 0);
    pio_sm_init(RIA_ACTION_PIO, RIA_ACTION_SM, offset, &config);
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

    // check on watchdog
    if (ria_action_in_progress())
    {
        absolute_time_t now = get_absolute_time();
        if (absolute_time_diff_us(now, action_watchdog_timer) < 0)
        {
            ria_stop();
            action_result = -2;
        }
    }
}

void ria_action_jmp(uint16_t addr)
{
    action_result = -1;
    ria_stop();
    // Reset vector
    saved_reset_vec = addr;
    REGSW(0xFFFC) = 0xFFF0;
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

static void read_or_verify_setup(uint16_t addr, uint16_t len, bool verify)
{
    if (!len)
        return;
    // Reset vector
    saved_reset_vec = REGSW(0xFFFC);
    REGSW(0xFFFC) = 0xFFF0;
    // Self-modifying fast load
    // FFF0  AD 00 00  LDA $0000
    // FFF3  8D FC FF  STA $FFFC/$FFFD
    // FFF6  80 F8     BRA $FFF0
    // FFF8  80 FE     BRA $FFF8
    REGS(0xFFF0) = 0xAD;
    REGS(0xFFF1) = addr & 0xFF;
    REGS(0xFFF2) = addr >> 8;
    REGS(0xFFF3) = 0x8D;
    REGS(0xFFF4) = verify ? 0xFC : 0xFD;
    REGS(0xFFF5) = 0xFF;
    REGS(0xFFF6) = 0x80;
    REGS(0xFFF7) = 0xF8;
    REGS(0xFFF8) = 0x80;
    REGS(0xFFF9) = 0xFE;
    rw_end = len;
    rw_pos = 0;
    if (verify)
        action_state = action_state_verify;
    else
        action_state = action_state_read;
    ria_reset();
}

void ria_action_ram_read(uint16_t addr, uint8_t *buf, uint16_t len)
{
    action_result = -1;
    ria_stop();
    // avoid forbidden areas
    while (len && (addr + len > 0xFFF0))
        if (addr + --len <= 0xFFFF)
            buf[len] = REGS(addr + len);
        else
            buf[len] = 0;
    while (len && (addr + len > 0xFF00))
        if (addr + --len <= 0xFFFF)
            buf[len] = 0;
    read_buf = buf;
    read_or_verify_setup(addr, len, false);
}

static inline void __force_inline ram_read(uint32_t data)
{
    if (rw_pos < rw_end)
    {
        REGSW(0xFFF1) += 1;
        read_buf[rw_pos] = data;
        if (++rw_pos == rw_end)
        {
            REGS(0xFFF7) = 0x00;
            ria_done();
        }
    }
}

void ria_action_ram_verify(uint16_t addr, const uint8_t *buf, uint16_t len)
{
    action_result = -1;
    ria_stop();
    // avoid forbidden areas
    while (len && (addr + len > 0xFFFA))
        if (addr + --len <= 0xFFFF && buf[len] != REGS(addr + len))
            action_result = addr + len;
    while (len && (addr + len > 0xFF00))
        --len;
    if (action_result != -1)
        return;
    write_buf = buf;
    read_or_verify_setup(addr, len, true);
}

static inline void __force_inline ram_verify(uint32_t data)
{
    if (rw_pos < rw_end)
    {
        REGSW(0xFFF1) += 1;
        if (write_buf[rw_pos] != data && action_result < 0)
            action_result = REGSW(0xFFF1) - 1;
        if (++rw_pos == rw_end)
        {
            REGS(0xFFF7) = 0x00;
            ria_done();
        }
    }
}

void ria_action_ram_write(uint16_t addr, const uint8_t *buf, uint16_t len)
{
    action_result = -1;
    ria_stop();
    // avoid forbidden area
    while (len && (addr + len > 0xFFF0))
        if (addr + --len <= 0xFFFF)
            REGS(addr + len) = buf[len];
    while (len && (addr + len > 0xFF00))
        len--;
    saved_reset_vec = REGSW(0xFFFC);
    if (!len)
        return;
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
    action_state = action_state_write;
    write_buf = buf;
    rw_end = len;
    // Evil hack because the first few writes with
    // a slow clock (1 kHz) won't actually write to SRAM.
    // This should be investigated further.
    rw_pos = -2;
    ria_reset();
}

static inline void __force_inline ram_write()
{
    if (rw_pos < rw_end)
    {
        if (rw_pos > 0)
        {
            REGS(0xFFF1) = write_buf[rw_pos];
            REGSW(0xFFF3) += 1;
        }
        if (++rw_pos == rw_end)
            REGS(0xFFF6) = 0x00;
    }
    else
        ria_done();
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
                if (action_state)
                    switch (addr)
                    {
                    case 0x16:
                        ram_write();
                        break;
                    case 0x1D:
                        ram_read(data);
                        break;
                    case 0x1C:
                        ram_verify(data);
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
