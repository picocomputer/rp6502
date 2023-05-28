/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "ria.h"
#include "pix.h"
#include "act.h"
#include "api.h"
#include "cpu.h"
#include "dev/com.h"
#include "ria.pio.h"
#include "mem/regs.h"
#include "mem/mbuf.h"
#include "mem/xram.h"
#include "mem/xstack.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include <stdio.h>

// This is the smallest value that will
// allow 1k read/write operations at 50 kHz.
#define ACT_WATCHDOG_MS 250

#define ACT_PIO pio1
#define ACT_SM 0

static enum state {
    action_state_idle = 0,
    action_state_read,
    action_state_write,
    action_state_verify,
} volatile action_state = action_state_idle;
static absolute_time_t action_watchdog_timer;
static volatile int32_t action_result = -1;
static int32_t saved_reset_vec = -1;
static uint16_t rw_addr;
static volatile int32_t rw_pos;
static volatile int32_t rw_end;

// RIA action has one variable read address.
static void act_set_watch_address(uint32_t addr)
{
    pio_sm_put(ACT_PIO, ACT_SM, addr & 0x1F);
}

void act_run()
{
    if (action_state == action_state_idle)
        return;
    action_result = -1;
    saved_reset_vec = REGSW(0xFFFC);
    REGSW(0xFFFC) = 0xFFF0;
    action_watchdog_timer = delayed_by_us(get_absolute_time(),
                                          cpu_get_reset_us() +
                                              ACT_WATCHDOG_MS * 1000);
    switch (action_state)
    {
    case action_state_write:
        // Self-modifying fast load
        // FFF0  A9 00     LDA #$00
        // FFF2  8D 00 00  STA $0000
        // FFF5  80 F9     BRA $FFF0
        // FFF7  EA        NOP
        // FFF8  80 FE     BRA $FFF8
        REGS(0xFFF0) = 0xA9;
        REGS(0xFFF1) = mbuf[0];
        REGS(0xFFF2) = 0x8D;
        REGS(0xFFF3) = rw_addr & 0xFF;
        REGS(0xFFF4) = rw_addr >> 8;
        REGS(0xFFF5) = 0x80;
        REGS(0xFFF6) = 0xF9;
        REGS(0xFFF7) = 0xEA;
        REGS(0xFFF8) = 0x80;
        REGS(0xFFF9) = 0xFE;
        break;
    case action_state_read:
    case action_state_verify:
        // Self-modifying fast load
        // FFF0  AD 00 00  LDA $0000
        // FFF3  8D FC FF  STA $FFFC/$FFFD
        // FFF6  80 F8     BRA $FFF0
        // FFF8  80 FE     BRA $FFF8
        REGS(0xFFF0) = 0xAD;
        REGS(0xFFF1) = rw_addr & 0xFF;
        REGS(0xFFF2) = rw_addr >> 8;
        REGS(0xFFF3) = 0x8D;
        REGS(0xFFF4) = (action_state == action_state_verify) ? 0xFC : 0xFD;
        REGS(0xFFF5) = 0xFF;
        REGS(0xFFF6) = 0x80;
        REGS(0xFFF7) = 0xF8;
        REGS(0xFFF8) = 0x80;
        REGS(0xFFF9) = 0xFE;
        break;
    default:
        break;
    }
}

void act_stop()
{
    action_state = action_state_idle;
    act_set_watch_address(0xFFE2);
    if (saved_reset_vec >= 0)
    {
        REGSW(0xFFFC) = saved_reset_vec;
        saved_reset_vec = -1;
    }
}

bool act_in_progress()
{
    return action_state != action_state_idle;
}

void act_task()
{
    // Report unexpected FIFO overflows and underflows
    // TODO needs much improvement
    uint32_t fdebug = ACT_PIO->fdebug;
    uint32_t masked_fdebug = fdebug & 0x0F0F0F0F; // reserved
    masked_fdebug &= ~(1 << (24 + ACT_SM));       // expected
    masked_fdebug &= ~(1 << (24 + PIX_SM));       // expected
    if (masked_fdebug)
    {
        ACT_PIO->fdebug = 0xFF;
        printf("ACT_PIO->fdebug: %lX\n", fdebug);
    }

    // check on watchdog
    if (act_in_progress())
    {
        absolute_time_t now = get_absolute_time();
        if (absolute_time_diff_us(now, action_watchdog_timer) < 0)
        {
            action_result = -2;
            main_stop();
        }
    }
}

bool act_error_message()
{
    switch (action_result)
    {
    case -1: // OK
        return false;
        break;
    case -2:
        printf("?watchdog timeout\n");
        break;
    default:
        printf("?verify failed at $%04lX\n", action_result);
        break;
    }
    return true;
}

void act_ram_read(uint16_t addr)
{
    // avoid forbidden areas
    uint16_t len = mbuf_len;
    while (len && (addr + len > 0xFFF0))
        if (addr + --len <= 0xFFFF)
            mbuf[len] = REGS(addr + len);
        else
            mbuf[len] = 0;
    while (len && (addr + len > 0xFF00))
        if (addr + --len <= 0xFFFF)
            mbuf[len] = 0;

    if (!len)
        return;
    rw_addr = addr;
    rw_end = len;
    rw_pos = 0;
    action_state = action_state_read;
    main_run();
}

void act_ram_verify(uint16_t addr)
{
    // avoid forbidden areas
    uint16_t len = mbuf_len;
    while (len && (addr + len > 0xFFFA))
        if (addr + --len <= 0xFFFF && mbuf[len] != REGS(addr + len))
            action_result = addr + len;
    while (len && (addr + len > 0xFF00))
        --len;
    if (action_result != -1)
        return;

    if (!len)
        return;
    rw_addr = addr;
    rw_end = len;
    rw_pos = 0;
    action_state = action_state_verify;
    main_run();
}

void act_ram_write(uint16_t addr)
{
    // avoid forbidden area
    uint16_t len = mbuf_len;
    while (len && (addr + len > 0xFFF0))
        if (addr + --len <= 0xFFFF)
            REGS(addr + len) = mbuf[len];
    while (len && (addr + len > 0xFF00))
        len--;
    if (!len)
        return;

    act_set_watch_address(0xFFF6);
    rw_addr = addr;
    rw_end = len;
    // Evil hack because the first few writes with
    // a slow clock (1 kHz) won't actually write to SRAM.
    // This should be investigated further.
    rw_pos = -2;
    action_state = action_state_write;
    main_run();
}

static void act_exit()
{
    gpio_put(CPU_RESB_PIN, false);
    main_stop();
}

#define CASE_READ(addr) (addr & 0x1F)
#define CASE_WRITE(addr) (0x20 | (addr & 0x1F))
static __attribute__((optimize("O1"))) void act_loop()
{
    // In here we bypass the usual SDK calls as needed for performance.
    while (true)
    {
        if (!(ACT_PIO->fstat & (1u << (PIO_FSTAT_RXEMPTY_LSB + ACT_SM))))
        {
            uint32_t rw_addr_data = ACT_PIO->rxf[ACT_SM];
            if (((1u << CPU_RESB_PIN) & sio_hw->gpio_in))
            {
                uint32_t data = rw_addr_data & 0xFF;
                switch (rw_addr_data >> 8)
                {
                case CASE_READ(0xFFF6): // action write
                    if (rw_pos < rw_end)
                    {
                        if (rw_pos > 0)
                        {
                            REGS(0xFFF1) = mbuf[rw_pos];
                            REGSW(0xFFF3) += 1;
                        }
                        if (++rw_pos == rw_end)
                            REGS(0xFFF6) = 0x00;
                    }
                    else
                        act_exit();
                    break;
                case CASE_WRITE(0xFFFD): // action read
                    if (rw_pos < rw_end)
                    {
                        REGSW(0xFFF1) += 1;
                        mbuf[rw_pos] = data;
                        if (++rw_pos == rw_end)
                        {
                            REGS(0xFFF7) = 0x00;
                            act_exit();
                        }
                    }
                    break;
                case CASE_WRITE(0xFFFC): // action verify
                    if (rw_pos < rw_end)
                    {
                        REGSW(0xFFF1) += 1;
                        if (mbuf[rw_pos] != data && action_result < 0)
                            action_result = REGSW(0xFFF1) - 1;
                        if (++rw_pos == rw_end)
                        {
                            REGS(0xFFF7) = 0x00;
                            act_exit();
                        }
                    }
                    break;
                case CASE_WRITE(0xFFEF): // OS function call
                    api_return_blocked();
                    if (data == 0x00) // zxreset()
                    {
                        xstack_ptr = XSTACK_SIZE;
                        API_STACK = xstack[xstack_ptr];
                        api_return_ax(0);
                    }
                    else if (data == 0xFF) // exit()
                        act_exit();
                    break;
                case CASE_WRITE(0xFFEC): // xstack
                    if (xstack_ptr)
                        xstack[--xstack_ptr] = data;
                    API_STACK = xstack[xstack_ptr];
                    break;
                case CASE_READ(0xFFEC): // xstack
                    if (xstack_ptr < XSTACK_SIZE)
                        ++xstack_ptr;
                    API_STACK = xstack[xstack_ptr];
                    break;
                case CASE_WRITE(0xFFEB): // Set XRAM >ADDR1
                    REGS(0xFFEB) = data;
                    XRAM_RW1 = xram[XRAM_ADDR1];
                    break;
                case CASE_WRITE(0xFFEA): // Set XRAM <ADDR1
                    REGS(0xFFEA) = data;
                    XRAM_RW1 = xram[XRAM_ADDR1];
                    break;
                case CASE_WRITE(0xFFE8): // W XRAM1
                    xram[XRAM_ADDR1] = data;
                    PIX_PIO->txf[PIX_SM] = XRAM_ADDR1 | (data << 16) | PIX_XRAM;
                    XRAM_RW0 = xram[XRAM_ADDR0];
                    __attribute__((fallthrough));
                case CASE_READ(0xFFE8): // R XRAM1
                    XRAM_ADDR1 += XRAM_STEP1;
                    XRAM_RW1 = xram[XRAM_ADDR1];
                    break;
                case CASE_WRITE(0xFFE7): // Set XRAM >ADDR0
                    REGS(0xFFE7) = data;
                    XRAM_RW0 = xram[XRAM_ADDR0];
                    break;
                case CASE_WRITE(0xFFE6): // Set XRAM <ADDR0
                    REGS(0xFFE6) = data;
                    XRAM_RW0 = xram[XRAM_ADDR0];
                    break;
                case CASE_WRITE(0xFFE4): // W XRAM0
                    xram[XRAM_ADDR0] = data;
                    PIX_PIO->txf[PIX_SM] = XRAM_ADDR0 | (data << 16) | PIX_XRAM;
                    XRAM_RW1 = xram[XRAM_ADDR1];
                    __attribute__((fallthrough));
                case CASE_READ(0xFFE4): // R XRAM0
                    XRAM_ADDR0 += XRAM_STEP0;
                    XRAM_RW0 = xram[XRAM_ADDR0];
                    break;
                case CASE_READ(0xFFE2): // UART Rx
                {
                    int ch = ria_uart_rx_char;
                    if (ch >= 0)
                    {
                        REGS(0xFFE2) = ch;
                        REGS(0xFFE0) |= 0b01000000;
                        ria_uart_rx_char = -1;
                    }
                    else
                    {
                        REGS(0xFFE0) &= ~0b01000000;
                        REGS(0xFFE2) = 0;
                    }
                    break;
                }
                case CASE_WRITE(0xFFE1): // UART Tx
                    uart_get_hw(COM_UART)->dr = data;
                    if ((uart_get_hw(COM_UART)->fr & UART_UARTFR_TXFF_BITS))
                        REGS(0xFFE0) &= ~0b10000000;
                    else
                        REGS(0xFFE0) |= 0b10000000;
                    break;
                case CASE_READ(0xFFE0): // UART Tx/Rx flow control
                {
                    int ch = ria_uart_rx_char;
                    if (!(REGS(0xFFE0) & 0b01000000) && ch >= 0)
                    {
                        REGS(0xFFE2) = ch;
                        REGS(0xFFE0) |= 0b01000000;
                        ria_uart_rx_char = -1;
                    }
                    if ((uart_get_hw(COM_UART)->fr & UART_UARTFR_TXFF_BITS))
                        REGS(0xFFE0) &= ~0b10000000;
                    else
                        REGS(0xFFE0) |= 0b10000000;
                    break;
                }
                }
            }
        }
    }
}

void act_init()
{
    // PIO to supply action loop with events
    uint offset = pio_add_program(ACT_PIO, &ria_action_program);
    pio_sm_config config = ria_action_program_get_default_config(offset);
    sm_config_set_in_pins(&config, RIA_PIN_BASE);
    sm_config_set_in_shift(&config, true, true, 32);
    pio_sm_init(ACT_PIO, ACT_SM, offset, &config);
    act_stop();
    pio_sm_set_enabled(ACT_PIO, ACT_SM, true);
    multicore_launch_core1(act_loop);
}

void act_reclock(uint16_t clkdiv_int, uint8_t clkdiv_frac)
{
    pio_sm_set_clkdiv_int_frac(ACT_PIO, ACT_SM, clkdiv_int, clkdiv_frac);
}
