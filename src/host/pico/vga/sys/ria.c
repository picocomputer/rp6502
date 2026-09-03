/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "vga.pio.h"
#include "vga/sys/com.h"
#include "core/sys/version.h"
#include "vga/sys/ria.h"
#include <pico/stdlib.h>
#include <hardware/clocks.h>
#include <hardware/sync.h>
#include <string.h>
#include <stdio.h>

/* The version answer, sent a byte a pass. Two segments because the board name
 * is this file's and the stamp is version.c's; version_tail is what follows
 * once version_pos runs out. */
static const char *version_pos;
static const char *version_tail;

// One byte on the wire, start bit through stop bit. A transmission must not
// straddle the window; its data bits carry edges the RIA would read as VSYNC.
#define RIA_BACKCHAN_BYTE_US (11 * 1000000 / RIA_BACKCHAN_BAUDRATE)

// ria_vsync runs on either core, ria_ack/ria_nak on core 0. Without this the
// check could pass on one core while the other queues VSYNC ahead of us.
static spin_lock_t *ria_lock;
static uint32_t ria_vsync_us;
static bool ria_vsync_seen;
static uint8_t ria_held; // 0 when nothing is waiting for the window to clear

// Is a VSYNC expected close enough that nothing else may be sent?
static bool ria_locked_out(void)
{
    if (!ria_vsync_seen)
        return false;
    // Unsigned wrap keeps this correct across the 32 bit rollover. A byte
    // started now must finish before the window opens, and nothing may start
    // until it closes.
    uint32_t since = time_us_32() - ria_vsync_us;
    return since + RIA_BACKCHAN_BYTE_US >= RIA_VSYNC_PERIOD_US - RIA_VSYNC_LOCKOUT_US &&
           since <= RIA_VSYNC_PERIOD_US + RIA_VSYNC_LOCKOUT_US;
}

static void ria_send(uint8_t byte)
{
    uint32_t save = spin_lock_blocking(ria_lock);
    if (ria_locked_out())
        ria_held = byte;
    else
        pio_sm_put(RIA_BACKCHAN_PIO, RIA_BACKCHAN_SM, byte);
    spin_unlock(ria_lock, save);
}

void ria_init(void)
{
    ria_lock = spin_lock_init(spin_lock_claim_unused(true));
    gpio_pull_up(RIA_BACKCHAN_PIN);
    pio_sm_set_pins_with_mask(RIA_BACKCHAN_PIO, RIA_BACKCHAN_SM, 1u << RIA_BACKCHAN_PIN, 1u << RIA_BACKCHAN_PIN);
    pio_sm_set_pindirs_with_mask(RIA_BACKCHAN_PIO, RIA_BACKCHAN_SM, 1u << RIA_BACKCHAN_PIN, 1u << RIA_BACKCHAN_PIN);
    uint offset = pio_add_program(RIA_BACKCHAN_PIO, &ria_backchannel_tx_program);
    pio_sm_config c = ria_backchannel_tx_program_get_default_config(offset);
    sm_config_set_out_shift(&c, true, false, 32);
    sm_config_set_out_pins(&c, RIA_BACKCHAN_PIN, 1);
    sm_config_set_sideset_pins(&c, RIA_BACKCHAN_PIN);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    float div = (float)clock_get_hz(clk_sys) / (8 * RIA_BACKCHAN_BAUDRATE);
    sm_config_set_clkdiv(&c, div);
    pio_sm_init(RIA_BACKCHAN_PIO, RIA_BACKCHAN_SM, offset, &c);
    pio_sm_set_enabled(RIA_BACKCHAN_PIO, RIA_BACKCHAN_SM, true);
}

void ria_task(void)
{
    if (version_pos && pio_sm_is_tx_fifo_empty(RIA_BACKCHAN_PIO, RIA_BACKCHAN_SM))
    {
        char ch = *version_pos++;
        if (!ch && version_tail)
        {
            version_pos = version_tail;
            version_tail = NULL;
            ch = *version_pos++;
        }
        if (!ch)
        {
            ch = '\r';
            version_pos = NULL;
        }
        // Version bytes predate the RIA arming its trap, so they skip the lockout.
        pio_sm_put(RIA_BACKCHAN_PIO, RIA_BACKCHAN_SM, ch);
    }

    if (ria_held)
    {
        uint32_t save = spin_lock_blocking(ria_lock);
        if (ria_held && !ria_locked_out())
        {
            pio_sm_put(RIA_BACKCHAN_PIO, RIA_BACKCHAN_SM, ria_held);
            ria_held = 0;
        }
        spin_unlock(ria_lock, save);
    }
}

void ria_pre_reclock(void)
{
    // Wait for empty FIFO
    while (pio_sm_get_tx_fifo_level(RIA_BACKCHAN_PIO, RIA_BACKCHAN_SM))
        tight_loop_contents();
    // Wait for shift register too (11 bit times, the trailing stop bit included)
    busy_wait_us_32(11 * 1000000 / RIA_BACKCHAN_BAUDRATE);
}

void ria_post_reclock(void)
{
    float div = (float)clock_get_hz(clk_sys) / (8 * RIA_BACKCHAN_BAUDRATE);
    pio_sm_set_clkdiv(RIA_BACKCHAN_PIO, RIA_BACKCHAN_SM, div);
}

void ria_backchan(uint16_t word)
{
    switch (word)
    {
    case 0: // detach backchannel (restore UART function on pin)
        gpio_set_function(RIA_BACKCHAN_PIN, GPIO_FUNC_UART);
        ria_vsync_seen = false;
        ria_held = 0;
        break;
    case 1: // attach backchannel (PIO drives pin) and queue version string
        pio_gpio_init(RIA_BACKCHAN_PIO, RIA_BACKCHAN_PIN);
        version_pos = "VGA ";
        version_tail = version_string();
        break;
    case 2: // reply to identification request
        /* STR_VGA1 is the RIA's half of this; spelled out because a board with
         * no string table should not gain one for four bytes. */
        uart_write_blocking(COM_UART_INTERFACE, (uint8_t *)"VGA1", sizeof "VGA1" - 1);
        break;
    }
}

void ria_vsync(void)
{
    static uint32_t frame_no;
    uint32_t save = spin_lock_blocking(ria_lock);
    pio_sm_put(RIA_BACKCHAN_PIO, RIA_BACKCHAN_SM, (++frame_no & 0xF) | 0x80);
    ria_vsync_us = time_us_32();
    ria_vsync_seen = true;
    spin_unlock(ria_lock, save);
}

void ria_ack(void)
{
    ria_send(0x90);
}

void ria_nak(void)
{
    ria_send(0xA0);
}
