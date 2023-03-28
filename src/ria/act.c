/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ria.h"
#include "act.h"
#include "api.h"
#include "dev/com.h"
#include "ria.pio.h"
#include "mem/regs.h"
#include "mem/mbuf.h"
#include "mem/vram.h"
#include "mem/vstack.h"
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include <stdio.h>

// This is the smallest value that will
// allow 1k read/write operations at 50 kHz.
#define RIA_ACTION_WATCHDOG_MS 250

static enum state {
    action_state_idle = 0,
    action_state_read,
    action_state_write,
    action_state_verify,
    action_state_exit
} volatile action_state = action_state_idle;
static absolute_time_t action_watchdog_timer;
static volatile int32_t action_result = -1;
static int32_t saved_reset_vec = -1;
static volatile int32_t rw_pos;
static volatile int32_t rw_end;

// RIA action has one variable read address.
static void act_set_address(uint32_t addr)
{
    pio_sm_put(RIA_ACTION_PIO, RIA_ACTION_SM, addr & 0x1F);
}

// -1 good, -2 timeout, >=0 failed verify at address
int32_t act_result()
{
    return action_result;
}

void act_reset()
{
    action_state = action_state_idle;
    act_set_address(0xFFE2);
    if (saved_reset_vec >= 0)
    {
        REGSW(0xFFFC) = saved_reset_vec;
        saved_reset_vec = -1;
    }
}

static void act_start(enum state state)
{
    saved_reset_vec = REGSW(0xFFFC);
    REGSW(0xFFFC) = 0xFFF0;
    action_state = state;
    action_watchdog_timer = delayed_by_us(get_absolute_time(),
                                          ria_get_reset_us() +
                                              RIA_ACTION_WATCHDOG_MS * 1000);
    ria_reset();
}

// This will call act_reset() in the next task loop.
// It's a safe way for cpu1 to stop the 6502.
static void act_exit()
{
    action_state = action_state_exit;
    ria_exit();
}

bool act_in_progress()
{
    return action_state != action_state_idle && action_state != action_state_exit;
}

void act_pio_init()
{
    // PIO to supply action loop with events
    uint offset = pio_add_program(RIA_ACTION_PIO, &ria_action_program);
    pio_sm_config config = ria_action_program_get_default_config(offset);
    sm_config_set_in_pins(&config, RIA_PIN_BASE);
    sm_config_set_in_shift(&config, true, true, 32);
    pio_sm_init(RIA_ACTION_PIO, RIA_ACTION_SM, offset, &config);
    act_reset();
    pio_sm_set_enabled(RIA_ACTION_PIO, RIA_ACTION_SM, true);
}

void act_task()
{
    // Report unexpected FIFO overflows and underflows
    // TODO needs much improvement
    uint32_t fdebug = RIA_ACTION_PIO->fdebug;
    uint32_t masked_fdebug = fdebug & 0x0F0F0F0F;  // reserved
    masked_fdebug &= ~(1 << (24 + RIA_ACTION_SM)); // expected
    masked_fdebug &= ~(1 << (24 + RIA_PIX_SM));    // expected
    if (masked_fdebug)
    {
        RIA_ACTION_PIO->fdebug = 0xFF;
        printf("RIA_ACTION_PIO->fdebug: %lX\n", fdebug);
    }

    if (action_state == action_state_exit)
        act_reset();

    // check on watchdog
    if (act_in_progress())
    {
        absolute_time_t now = get_absolute_time();
        if (absolute_time_diff_us(now, action_watchdog_timer) < 0)
        {
            act_reset();
            ria_stop();
            action_result = -2;
        }
    }
}

static void read_or_verify_setup(uint16_t addr, uint16_t len, bool verify)
{
    if (!len)
        return;
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
        act_start(action_state_verify);
    else
        act_start(action_state_read);
}

void act_ram_read(uint16_t addr)
{
    action_result = -1;
    ria_stop();
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
    read_or_verify_setup(addr, len, false);
}

void act_ram_verify(uint16_t addr)
{
    action_result = -1;
    ria_stop();
    // avoid forbidden areas
    uint16_t len = mbuf_len;
    while (len && (addr + len > 0xFFFA))
        if (addr + --len <= 0xFFFF && mbuf[len] != REGS(addr + len))
            action_result = addr + len;
    while (len && (addr + len > 0xFF00))
        --len;
    if (action_result != -1)
        return;
    read_or_verify_setup(addr, len, true);
}

void act_ram_write(uint16_t addr)
{
    action_result = -1;
    ria_stop();
    // avoid forbidden area
    uint16_t len = mbuf_len;
    while (len && (addr + len > 0xFFF0))
        if (addr + --len <= 0xFFFF)
            REGS(addr + len) = mbuf[len];
    while (len && (addr + len > 0xFF00))
        len--;
    if (!len)
        return;
    // Self-modifying fast load
    // FFF0  A9 00     LDA #$00
    // FFF2  8D 00 00  STA $0000
    // FFF5  80 F9     BRA $FFF0
    // FFF7  EA        NOP
    // FFF8  80 FE     BRA $FFF8
    REGS(0xFFF0) = 0xA9;
    REGS(0xFFF1) = mbuf[0];
    REGS(0xFFF2) = 0x8D;
    REGS(0xFFF3) = addr & 0xFF;
    REGS(0xFFF4) = addr >> 8;
    REGS(0xFFF5) = 0x80;
    REGS(0xFFF6) = 0xF9;
    REGS(0xFFF7) = 0xEA;
    REGS(0xFFF8) = 0x80;
    REGS(0xFFF9) = 0xFE;
    act_set_address(0xFFF6);
    rw_end = len;
    // Evil hack because the first few writes with
    // a slow clock (1 kHz) won't actually write to SRAM.
    // This should be investigated further.
    rw_pos = -2;
    act_start(action_state_write);
}

#define CASE_READ(addr) (addr & 0x1F)
#define CASE_WRITE(addr) (0x20 | (addr & 0x1F))
__attribute__((optimize("O1"))) void act_loop()
{
    // In here we bypass the usual SDK calls as needed for performance.
    while (true)
    {
        if (!(RIA_ACTION_PIO->fstat & (1u << (PIO_FSTAT_RXEMPTY_LSB + RIA_ACTION_SM))))
        {
            uint32_t rw_addr_data = RIA_ACTION_PIO->rxf[RIA_ACTION_SM];
            if (((1u << RIA_RESB_PIN) & sio_hw->gpio_in))
            {
                uint32_t data = rw_addr_data & 0xFF;
                uint32_t rw_addr = (rw_addr_data >> 8);
                switch (rw_addr)
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
                    if (data == 0x00) // zvreset()
                    {
                        vstack_ptr = VSTACK_SIZE;
                        API_STACK = vstack[vstack_ptr];
                        api_return_ax(0);
                    }
                    else if (data == 0xFF) // exit()
                        ria_exit();
                    break;
                case CASE_WRITE(0xFFEC): // vstack
                    if (vstack_ptr)
                        vstack[--vstack_ptr] = data;
                    API_STACK = vstack[vstack_ptr];
                    break;
                case CASE_READ(0xFFEC): // vstack
                    if (vstack_ptr < VSTACK_SIZE)
                        ++vstack_ptr;
                    API_STACK = vstack[vstack_ptr];
                    break;
                case CASE_WRITE(0xFFEB): // Set VRAM >ADDR1
                    REGS(0xFFEB) = data;
                    VRAM_RW1 = vram[VRAM_ADDR1];
                    break;
                case CASE_WRITE(0xFFEA): // Set VRAM <ADDR1
                    REGS(0xFFEA) = data;
                    VRAM_RW1 = vram[VRAM_ADDR1];
                    break;
                case CASE_WRITE(0xFFE8): // W VRAM1
                    vram[VRAM_ADDR1] = data;
                    RIA_PIX_PIO->txf[RIA_PIX_SM] = VRAM_ADDR1 | (data << 16) | RIA_PIX_VRAM;
                    __attribute__((fallthrough));
                case CASE_READ(0xFFE8): // R VRAM1
                    VRAM_ADDR1 += VRAM_STEP1;
                    VRAM_RW1 = vram[VRAM_ADDR1];
                    break;
                case CASE_WRITE(0xFFE7): // Set VRAM >ADDR0
                    REGS(0xFFE7) = data;
                    VRAM_RW0 = vram[VRAM_ADDR0];
                    break;
                case CASE_WRITE(0xFFE6): // Set VRAM <ADDR0
                    REGS(0xFFE6) = data;
                    VRAM_RW0 = vram[VRAM_ADDR0];
                    break;
                case CASE_WRITE(0xFFE4): // W VRAM0
                    vram[VRAM_ADDR0] = data;
                    RIA_PIX_PIO->txf[RIA_PIX_SM] = VRAM_ADDR0 | (data << 16) | RIA_PIX_VRAM;
                    __attribute__((fallthrough));
                case CASE_READ(0xFFE4): // R VRAM0
                    VRAM_ADDR0 += VRAM_STEP0;
                    VRAM_RW0 = vram[VRAM_ADDR0];
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
                    uart_get_hw(RIA_UART)->dr = data;
                    if ((uart_get_hw(RIA_UART)->fr & UART_UARTFR_TXFF_BITS))
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
                    if ((uart_get_hw(RIA_UART)->fr & UART_UARTFR_TXFF_BITS))
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
