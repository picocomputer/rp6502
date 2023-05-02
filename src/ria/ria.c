/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mon/mon.h"
#include "cfg.h"
#include "mem/regs.h"
#include "ria.h"
#include "act.h"
#include "api.h"
#include "dev/com.h"
#include "ria.pio.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/structs/bus_ctrl.h"
#include <stdio.h>

// RP6502 Interface Adapter for WDC W65C02S.

static absolute_time_t ria_reset_timer;
static enum state {
    ria_state_stopped,
    ria_state_reset,
    ria_state_run,
    ria_state_exit
} volatile ria_state;

// Stop the 6502
void ria_stop()
{
    gpio_put(RIA_RESB_PIN, false);
    ria_state = ria_state_stopped;
    ria_reset_timer = delayed_by_us(get_absolute_time(),
                                    ria_get_reset_us());
    api_stop();
}

// Start or reset the 6502
void ria_reset()
{
    if (ria_state != ria_state_stopped)
        ria_stop();
    ria_state = ria_state_reset;
    api_reset();
}

// This will call ria_stop() in the next task loop.
// It's a safe way for cpu1 to stop the 6502.
void ria_exit()
{
    gpio_put(RIA_RESB_PIN, false);
    ria_state = ria_state_exit;
}

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

void ria_pix_pio_init()
{
    uint offset = pio_add_program(RIA_PIX_PIO, &ria_pix_program);
    pio_sm_config config = ria_pix_program_get_default_config(offset);
    sm_config_set_out_pins(&config, 0, 4);
    sm_config_set_out_shift(&config, false, false, 32);
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);
    for (int i = 0; i < 4; i++)
        pio_gpio_init(RIA_PIX_PIO, i);
    pio_sm_set_consecutive_pindirs(RIA_PIX_PIO, RIA_PIX_SM, 0, 4, true);
    pio_sm_init(RIA_PIX_PIO, RIA_PIX_SM, offset, &config);
    pio_sm_put(RIA_PIX_PIO, RIA_PIX_SM, RIA_PIX_IDLE);
    pio_sm_exec_wait_blocking(RIA_PIX_PIO, RIA_PIX_SM, pio_encode_pull(false, true));
    pio_sm_exec_wait_blocking(RIA_PIX_PIO, RIA_PIX_SM, pio_encode_mov(pio_x, pio_osr));
    pio_sm_set_enabled(RIA_PIX_PIO, RIA_PIX_SM, true);
}

bool ria_is_active()
{
    return ria_state != ria_state_stopped;
}

// Set the 6502 clock frequency. 0=default.
// Returns quantized actual frequency.
uint32_t ria_set_phi2_khz(uint32_t freq_khz)
{
    if (!freq_khz)
        freq_khz = 4000;
    uint32_t sys_clk_khz = freq_khz * 30;
    uint32_t old_sys_clk_hz = clock_get_hz(clk_sys);
    uint16_t clkdiv_int = 1;
    uint8_t clkdiv_frac = 0;
    com_flush();
    if (sys_clk_khz < 120 * 1000)
    {
        // <=4MHz resolution is limited by the divider.
        sys_clk_khz = 120 * 1000;
        clkdiv_int = sys_clk_khz / 30 / freq_khz;
        clkdiv_frac = ((float)sys_clk_khz / 30 / freq_khz - clkdiv_int) * (1u << 8u);
        set_sys_clock_khz(sys_clk_khz, true);
    }
    else
        // >4MHz will clock the Pi Pico past 120MHz with no divider.
        while (!set_sys_clock_khz(sys_clk_khz, false))
            sys_clk_khz += 1;
    pio_sm_set_clkdiv_int_frac(RIA_ACTION_PIO, RIA_ACTION_SM, clkdiv_int, clkdiv_frac);
    pio_sm_set_clkdiv_int_frac(RIA_WRITE_PIO, RIA_WRITE_SM, clkdiv_int, clkdiv_frac);
    pio_sm_set_clkdiv_int_frac(RIA_READ_PIO, RIA_READ_SM, clkdiv_int, clkdiv_frac);
    pio_sm_set_clkdiv_int_frac(RIA_PIX_PIO, RIA_PIX_SM, clkdiv_int, clkdiv_frac);
    if (old_sys_clk_hz != clock_get_hz(clk_sys))
        com_init();
    return sys_clk_khz / 30 / (clkdiv_int + clkdiv_frac / 256.f);
}

// Return calculated reset time. May be higher than requested
// to guarantee the 6502 gets two clock cycles during reset.
uint32_t ria_get_reset_us()
{
    uint32_t reset_ms = cfg_get_reset_ms();
    uint32_t phi2_khz = cfg_get_phi2_khz();
    if (!reset_ms)
        return (2000000 / phi2_khz + 999) / 1000;
    if (phi2_khz == 1 && reset_ms == 1)
        return 2000;
    return reset_ms * 1000;
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

    // drive reset pin
    gpio_init(RIA_RESB_PIN);
    gpio_put(RIA_IRQB_PIN, false);
    gpio_set_dir(RIA_RESB_PIN, true);

    // drive irq pin
    gpio_init(RIA_IRQB_PIN);
    gpio_put(RIA_IRQB_PIN, true);
    gpio_set_dir(RIA_IRQB_PIN, true);

    // the inits
    ria_write_pio_init();
    ria_read_pio_init();
    ria_pix_pio_init();
    act_pio_init();
    // Force cfg to call ria_set_phi2_khz
    cfg_set_phi2_khz(cfg_get_phi2_khz());
    ria_stop();
    multicore_launch_core1(act_loop);
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

    // Reset timer
    if (ria_state == ria_state_reset)
    {
        absolute_time_t now = get_absolute_time();
        if (absolute_time_diff_us(now, ria_reset_timer) < 0)
        {
            ria_state = ria_state_run;
            gpio_put(RIA_RESB_PIN, true);
        }
    }

    // Stopping event
    if (ria_state == ria_state_exit)
        ria_stop();
}

bool ria_pix_ready()
{
    // PIX TX FIFO is joined to be 8 deep.
    // Need space for the one the caller is about to push
    // and any that might arrive from the action loop.
    // TODO The threshold could probably be "<7", need test.
    return pio_sm_get_tx_fifo_level(RIA_PIX_PIO, RIA_PIX_SM) < 6;
}

void ria_pix_send(uint8_t ch3, uint16_t hi12, uint16_t lo16)
{
    uint32_t data = RIA_PIX_XREG(ch3) | ((hi12 & 0x0FFFu) << 16) | lo16;
    pio_sm_put(RIA_PIX_PIO, RIA_PIX_SM, data);
}
