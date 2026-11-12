/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/api/xreg.h"
#include "core/sys/sys.h"
#include "core/api/api.h"
#include "core/api/std.h"
#include "ria/sys/pix.h"
#include "ria/sys/vga.h"
#include "core/sys/xram.h"
#include "ria.pio.h"
#include "machine.h"
#include <hardware/dma.h>
#include <pico/time.h>
#include <string.h>

static uint32_t pix_send_count;
static absolute_time_t pix_api_state_timer;
static uint pix_stream_dma;

// XRAM messages for PIX_STREAM_SM. The DMA wraps its read address at the
// ring's size, so the ring is aligned to it.
#define PIX_RING_SIZE 128
static uint32_t pix_ring[PIX_RING_SIZE]
    __attribute__((aligned(sizeof(uint32_t[PIX_RING_SIZE]))));

// This may be modified in an IRQ from vga.c
static volatile enum {
    pix_api_running,
    pix_api_waiting,
    pix_api_ack,
    pix_api_nak,
} pix_api_state;

// The VGA holds ack/nak out of the window around each VSYNC, so this must
// exceed that lockout plus a frame of scheduling slack.
#define PIX_ACK_TIMEOUT_MS 25

// Both state machines take the divider and restart it together, in lockstep.
void pix_reclock(uint16_t clkdiv_int, uint8_t clkdiv_frac)
{
    pio_sm_set_clkdiv_int_frac(PIX_PIO, PIX_SM, clkdiv_int, clkdiv_frac);
    pio_sm_set_clkdiv_int_frac(PIX_PIO, PIX_STREAM_SM, clkdiv_int, clkdiv_frac);
    pio_clkdiv_restart_sm_mask(PIX_PIO, (1u << PIX_SM) | (1u << PIX_STREAM_SM));
}

void HOST_IN_FLASH_FUNC pix_init(void)
{
    uint offset = pio_add_program(PIX_PIO, &pix_tx_program);
    pio_sm_config config = pix_tx_program_get_default_config(offset);
    sm_config_set_out_pins(&config, PIX_PIN_BASE, PIX_PIN_COUNT);
    sm_config_set_out_shift(&config, false, false, 32);
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);
    for (int i = 0; i < PIX_PIN_COUNT; i++)
        pio_gpio_init(PIX_PIO, PIX_PIN_BASE + i);
    pio_sm_set_consecutive_pindirs(PIX_PIO, PIX_SM, PIX_PIN_BASE, PIX_PIN_COUNT, true);

    // The stream SM sends IDLE from X when its FIFO is empty.
    pio_sm_init(PIX_PIO, PIX_STREAM_SM, offset + pix_tx_offset_token, &config);
    pio_sm_put(PIX_PIO, PIX_STREAM_SM, PIX_MESSAGE(PIX_DEVICE_IDLE, 0, 0, 0));
    pio_sm_exec_wait_blocking(PIX_PIO, PIX_STREAM_SM, pio_encode_pull(false, true));
    pio_sm_exec_wait_blocking(PIX_PIO, PIX_STREAM_SM, pio_encode_mov(pio_x, pio_osr));

    // The core SM starts at check with an empty OSR, so it hands over the first slot.
    sm_config_set_out_shift(&config, false, true, 32);
    sm_config_set_wrap(&config, offset + pix_tx_offset_check, offset + pix_tx_offset_token);
    pio_sm_init(PIX_PIO, PIX_SM, offset + pix_tx_offset_check, &config);
    pio_enable_sm_mask_in_sync(PIX_PIO, (1u << PIX_SM) | (1u << PIX_STREAM_SM));

    pix_stream_dma = dma_claim_unused_channel(true);
    dma_channel_config dma_config = dma_channel_get_default_config(pix_stream_dma);
    channel_config_set_dreq(&dma_config, pio_get_dreq(PIX_PIO, PIX_STREAM_SM, true));
    channel_config_set_ring(&dma_config, false, __builtin_ctz(sizeof(pix_ring)));
    dma_channel_configure(pix_stream_dma, &dma_config, &PIX_PIO->txf[PIX_STREAM_SM],
                          pix_ring, 0, false);

    // Queue two IDLE messages so the PIX bus re-syncs after init
    pix_send(PIX_DEVICE_IDLE, 0, 0, 0);
    pix_send(PIX_DEVICE_IDLE, 0, 0, 0);
}

void pix_stop(void)
{
    pix_send_count = 0;
    pix_api_state = pix_api_running;
}

void pix_ack(void)
{
    if (pix_api_state == pix_api_waiting)
        pix_api_state = pix_api_ack;
}

void pix_nak(void)
{
    if (pix_api_state == pix_api_waiting)
        pix_api_state = pix_api_nak;
}

void pix_wait_begin(uint32_t timeout_ms)
{
    pix_api_state = pix_api_waiting;
    pix_api_state_timer = make_timeout_time_ms(timeout_ms);
}

int pix_wait_poll(void)
{
    switch (pix_api_state)
    {
    case pix_api_ack:
        pix_api_state = pix_api_running;
        return 1;
    case pix_api_nak:
        pix_api_state = pix_api_running;
        return -1;
    case pix_api_waiting:
        if (time_reached(pix_api_state_timer))
        {
            pix_api_state = pix_api_running;
            return -2;
        }
        return 0;
    default:
        return -1;
    }
}

bool HOST_IN_FLASH_FUNC pix_api_xreg(void)
{
    static uint8_t pix_device;
    static uint8_t pix_channel;
    static uint8_t pix_index;

    switch (pix_api_state)
    {
    case pix_api_running:
        break;
    case pix_api_waiting:
        if (time_reached(pix_api_state_timer))
        {
            pix_api_state = pix_api_running;
            pix_send_count = 0;
            return api_return_errno(API_EIO);
        }
        return api_working();
    case pix_api_ack:
        if (!pix_ready())
            return api_working();
        pix_api_state = pix_api_running;
        if (pix_send_count == 0)
        {
            xstack_ptr = XSTACK_SIZE;
            return api_return_ax(0);
        }
        break;
    case pix_api_nak:
        pix_api_state = pix_api_running;
        pix_send_count = 0;
        return api_return_errno(API_EINVAL);
    }

    // In progress, send one xreg
    if (pix_send_count)
    {
        if (pix_ready())
        {
            --pix_send_count;
            uint16_t data = 0;
            if (!api_pop_uint16(&data))
            {
                pix_send_count = 0;
                return api_return_errno(API_EINVAL);
            }
            pix_send(pix_device, pix_channel, pix_index + pix_send_count, data);
            if (pix_device == PIX_DEVICE_VGA && pix_channel == 0 &&
                pix_index + pix_send_count == 0)
                vga_set_canvas(data);
            // VGA channel 0 regs 0 (canvas) and 1 (mode) require an ACK.
            if (pix_device == PIX_DEVICE_VGA && pix_channel == 0 &&
                pix_index + pix_send_count <= 1)
            {
                pix_api_state = pix_api_waiting;
                pix_api_state_timer = make_timeout_time_ms(PIX_ACK_TIMEOUT_MS);
            }
            else if (!pix_send_count)
                pix_api_state = pix_api_ack;
        }
        return api_working();
    }

    // Setup for new call
    pix_device = xstack[XSTACK_SIZE - 1];
    pix_channel = xstack[XSTACK_SIZE - 2];
    pix_index = xstack[XSTACK_SIZE - 3];
    pix_send_count = (XSTACK_SIZE - xstack_ptr - 3) / 2;
    if (!(xstack_ptr & 0x01) ||
        pix_send_count < 1 || pix_send_count > XSTACK_SIZE / 2 ||
        pix_index + pix_send_count > 0x100 ||
        pix_device > 7 || pix_channel > 15)
    {
        pix_send_count = 0;
        return api_return_errno(API_EINVAL);
    }

    // VGA control channel ($F) is RIA-private while VGA is connected.
    if (pix_device == PIX_DEVICE_VGA && pix_channel == 0xF && vga_connected())
    {
        pix_send_count = 0;
        return api_return_errno(API_EACCES);
    }

    // Local PIX device $0. Every register here is a key register with no
    // argument, so the burst is written in order. api_pop_uint16 yields the
    // highest address first, so the words are indexed instead of popped.
    if (pix_device == PIX_DEVICE_RIA)
    {
        for (uint32_t i = 0; i < pix_send_count; i++)
        {
            uint16_t data;
            memcpy(&data, &xstack[XSTACK_SIZE - 5 - 2 * i], sizeof(data));
            if (!xreg0(pix_channel, (uint8_t)(pix_index + i), data))
            {
                pix_send_count = 0;
                return api_return_errno(API_EINVAL);
            }
        }
        pix_send_count = 0;
        xstack_ptr = XSTACK_SIZE;
        return api_return_ax(0);
    }

    // Special case of sending VGA canvas and mode in same call.
    // Because we send in reverse, canvas has to be first or it'll clear mode programming.
    if (pix_device == PIX_DEVICE_VGA && pix_channel == 0 &&
        pix_index == 0 && pix_send_count > 1)
    {
        uint16_t canvas;
        memcpy(&canvas, &xstack[XSTACK_SIZE - 5], sizeof(canvas));
        pix_send_blocking(PIX_DEVICE_VGA, 0, 0, canvas);
        vga_set_canvas(canvas);
        pix_index = 1;
        pix_send_count -= 1;
        pix_api_state = pix_api_waiting;
        pix_api_state_timer = make_timeout_time_ms(PIX_ACK_TIMEOUT_MS);
    }
    return api_working();
}

// A CANVAS or DISPLAY can change the VGA's system clock.
// Its PIX receiver loses messages until the new clock settles.
#define PIX_HOLD_US 1000
static uint64_t pix_hold_end_us;

void HOST_IN_RAM_FUNC pix_send(uint8_t dev3, uint8_t ch4, uint8_t byte, uint16_t word)
{
    assert(dev3 < 8);
    assert(ch4 < 16);
    pio_sm_put(PIX_PIO, PIX_SM, PIX_MESSAGE(dev3, ch4, byte, word));
    if (dev3 == PIX_DEVICE_VGA && byte == 0 && (ch4 == 0 || ch4 == 0xF))
        pix_hold_end_us = time_us_64() + PIX_HOLD_US;
}

// PIX_SM's TX FIFO is joined. Core 1 puts the 6502's portal writes in it
// without checking for room, so the main loop leaves it PIX_CORE1_SLOTS.
#define PIX_FIFO_DEPTH 8
#define PIX_CORE1_SLOTS 2

bool HOST_IN_RAM_FUNC pix_ready(void)
{
    return time_us_64() >= pix_hold_end_us &&
           pio_sm_get_tx_fifo_level(PIX_PIO, PIX_SM) < PIX_FIFO_DEPTH - PIX_CORE1_SLOTS;
}

// A run starts only on an idle DMA, which has left its read address at the
// ring slot after the last word it read.
uint32_t HOST_IN_RAM_FUNC pix_stream_xram(uint32_t addr, uint32_t end)
{
    if (dma_channel_is_busy(pix_stream_dma))
        return addr;
    uint32_t count = end - addr;
    if (count > PIX_RING_SIZE)
        count = PIX_RING_SIZE;
    uint32_t slot = dma_hw->ch[pix_stream_dma].read_addr / sizeof(uint32_t);
    for (uint32_t i = 0; i < count; i++, addr++)
        pix_ring[slot++ % PIX_RING_SIZE] = PIX_MESSAGE(PIX_DEVICE_XRAM, 0, xram[addr], addr);
    dma_channel_set_transfer_count(pix_stream_dma, dma_encode_transfer_count(count), true);
    return addr;
}

// The stream SM keeps its slot until the last message it pulled is out.
bool HOST_IN_RAM_FUNC pix_stream_done(void)
{
    return !dma_channel_is_busy(pix_stream_dma) &&
           pio_sm_is_tx_fifo_empty(PIX_PIO, PIX_STREAM_SM);
}
