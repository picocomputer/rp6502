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

static uint pix_stream_dma;

// XRAM messages for PIX_STREAM_SM. The DMA wraps its read address at the
// ring's size, so the ring is aligned to it.
#define PIX_RING_SIZE 128
static uint32_t pix_ring[PIX_RING_SIZE]
    __attribute__((aligned(sizeof(uint32_t[PIX_RING_SIZE]))));

/* A 6502 program's xreg burst, across the calls of its API op. */
static enum {
    pix_api_idle,      // the next call starts a burst
    pix_api_sending,   // words of the burst are left to send
    pix_api_answering, // the VGA's answer to the word sent last is awaited
    pix_api_finishing, // every word is sent, and the op returns once PIX has room
} pix_api_state;
/* The burst, set when it starts and read only until it ends. */
static uint32_t pix_send_count;
static uint8_t pix_device;
static uint8_t pix_channel;
static uint8_t pix_index;

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
    pix_api_state = pix_api_idle;
}

static bool pix_api_fail(api_errno errnum)
{
    pix_api_state = pix_api_idle;
    return api_return_errno(errnum);
}

/* The burst's words go from the highest index down. VGA channel 0's CANVAS
 * and MODE are answered (vga.h): each is asked once the VGA may be asked,
 * and the burst waits for its answer. With no VGA connected they fail. */
static bool HOST_IN_FLASH_FUNC pix_api_send(void)
{
    uint8_t index = (uint8_t)(pix_index + pix_send_count - 1);
    bool answered = pix_device == PIX_DEVICE_VGA && pix_channel == 0 && index <= 1;
    if (!pix_ready() || (answered && !vga_ask_ready()))
        return api_working();
    uint16_t data = 0;
    if (!api_pop_uint16(&data))
        return pix_api_fail(API_EINVAL);
    --pix_send_count;
    if (!answered)
        pix_send(pix_device, pix_channel, index, data);
    else if (!vga_ask(0, index, data, VGA_ANSWER_MS))
        return pix_api_fail(API_EIO);
    if (pix_device == PIX_DEVICE_VGA && pix_channel == 0)
        vga_xreg(index, data);
    pix_api_state = answered         ? pix_api_answering
                    : pix_send_count ? pix_api_sending
                                     : pix_api_finishing;
    return api_working();
}

static bool HOST_IN_FLASH_FUNC pix_api_begin(void)
{
    pix_device = xstack[XSTACK_SIZE - 1];
    pix_channel = xstack[XSTACK_SIZE - 2];
    pix_index = xstack[XSTACK_SIZE - 3];
    pix_send_count = (XSTACK_SIZE - xstack_ptr - 3) / 2;
    if (!(xstack_ptr & 0x01) ||
        pix_send_count < 1 || pix_send_count > XSTACK_SIZE / 2 ||
        pix_index + pix_send_count > 0x100 ||
        pix_device >= PIX_DEVICE_IDLE || pix_channel > 15)
        return pix_api_fail(API_EINVAL);

    // VGA control channel ($F) is RIA-private while VGA is connected.
    if (pix_device == PIX_DEVICE_VGA && pix_channel == 0xF && vga_connected())
        return pix_api_fail(API_EACCES);

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
                return pix_api_fail(API_EINVAL);
        }
        xstack_ptr = XSTACK_SIZE;
        return api_return_ax(0);
    }

    // Special case of sending VGA canvas and mode in same call.
    // Because we send in reverse, canvas has to be first or it'll clear mode
    // programming. Until it can go, the next call starts the burst again.
    if (pix_device == PIX_DEVICE_VGA && pix_channel == 0 &&
        pix_index == 0 && pix_send_count > 1)
    {
        if (!pix_ready() || !vga_ask_ready())
            return api_working();
        uint16_t canvas;
        memcpy(&canvas, &xstack[XSTACK_SIZE - 5], sizeof(canvas));
        if (!vga_ask(0, 0, canvas, VGA_ANSWER_MS))
            return pix_api_fail(API_EIO);
        vga_xreg(0, canvas);
        pix_index = 1;
        pix_send_count -= 1;
        pix_api_state = pix_api_answering;
        return api_working();
    }
    pix_api_state = pix_api_sending;
    return api_working();
}

/* A VGA that does not answer in time is lost (vga.h), and the op fails. */
bool HOST_IN_FLASH_FUNC pix_api_xreg(void)
{
    switch (pix_api_state)
    {
    case pix_api_idle:
        return pix_api_begin();
    case pix_api_sending:
        return pix_api_send();
    case pix_api_answering:
        switch (vga_answer())
        {
        case VGA_ANSWER_WAITING:
            return api_working();
        case VGA_ANSWER_NAK:
            return pix_api_fail(API_EINVAL);
        case VGA_ANSWER_TIMEOUT:
            return pix_api_fail(API_EIO);
        case VGA_ANSWER_ACK:
            break;
        }
        if (pix_send_count)
            return pix_api_send();
        pix_api_state = pix_api_finishing;
        __attribute__((fallthrough));
    case pix_api_finishing:
        if (!pix_ready())
            return api_working();
        pix_api_state = pix_api_idle;
        xstack_ptr = XSTACK_SIZE;
        return api_return_ax(0);
    }
    return api_working();
}

// A CANVAS or DISPLAY can change the VGA's system clock, and its PIX
// receiver loses messages until the new clock settles. The VGA answers
// either before its main loop changes the clock, and the pointer goes while
// an answer is awaited, so the answer does not cover this hold.
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
