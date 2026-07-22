/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "ria/sys/pix.h"
#include "emu/main.h"
#include "ria/api/api.h"
#include "ria/api/std.h"
#include <string.h>

/* The receiver side of the PIX bus, collapsed into the emu. The on-device driver
 * (ria/sys/pix.h) fires messages into a PIO FIFO; the host has none, so the
 * <hardware/pio.h> shim's pio_sm_put delivers each one immediately here. pio1 is
 * the PIX PIO the driver targets — unused on the host, but its address resolves. */

static pio_hw_t pix_pio;
pio_hw_t *const pio1 = &pix_pio;

/* Deliver one PIX message. Device 0 (XRAM) is the shared xram[], already
 * satisfied, so dropped; VGA goes to the xreg dispatch; 2-7 have no emu hardware.
 * Returns the VGA's ACK/NAK (true otherwise). The RIA-local device-0 xreg is a
 * virtual pre-bus device handled in pix_api_xreg via main_xreg_0. */
static bool pix_deliver(uint8_t dev, uint8_t channel, uint8_t byte, uint16_t word)
{
    switch (dev)
    {
    case PIX_DEVICE_XRAM: /* == PIX_DEVICE_RIA: shared xram, dropped */
        return true;
    case PIX_DEVICE_VGA:
        return main_xreg_1(channel, byte, word);
    default: /* devices 2-7: over the bus, no emu hardware, dropped */
        return true;
    }
}

/* pix.h's pix_send packs a PIX_MESSAGE and puts it here; unpack and deliver. */
void pio_sm_put(pio_hw_t *pio, unsigned sm, uint32_t msg)
{
    (void)pio;
    (void)sm;
    pix_deliver((msg >> 29) & 0x07, (msg >> 24) & 0x0F,
                (msg >> 16) & 0xFF, msg & 0xFFFF);
}

/* The i-th xreg data word (target address+i) sits at xstack[SIZE-5-2i]. */
static uint16_t word_at(int i)
{
    uint16_t word;
    memcpy(&word, &xstack[XSTACK_SIZE - 5 - 2 * i], sizeof(word));
    return word;
}

bool pix_api_xreg(void)
{
    uint8_t device = xstack[XSTACK_SIZE - 1];
    uint8_t channel = xstack[XSTACK_SIZE - 2];
    uint8_t address = xstack[XSTACK_SIZE - 3];
    int count = (int)((XSTACK_SIZE - xstack_ptr - 3) / 2);
    bool aligned = (xstack_ptr & 1) != 0;
    xstack_ptr = XSTACK_SIZE; /* args consumed; nothing below reads xstack_ptr */
    if (!aligned || count < 1 || count > XSTACK_SIZE / 2 ||
        device > 7 || channel > 15)
        return api_return_errno(API_EINVAL);
    /* VGA control channel ($F) is RIA-private while VGA is connected (always,
     * in the emulator), so a write NAKs (mirrors ria/sys/pix.c). */
    if (device == PIX_DEVICE_VGA && channel == 0xF)
        return api_return_errno(API_EACCES);
    /* Device 0 is the RIA-local virtual xreg, never bussed: dispatch straight to
     * main_xreg_0 with the address held constant (last-wins). */
    if (device == PIX_DEVICE_RIA)
    {
        for (int i = count - 1; i >= 0; i--)
            if (!main_xreg_0(channel, address, word_at(i)))
                return api_return_errno(API_EINVAL);
        return api_return_ax(0);
    }
    /* A VGA channel-0 write from address 0 must send the canvas word (address 0)
     * first so it can't clear later mode programming; the rest follow high
     * address -> low, landing each register after the parameters it consumes
     * (e.g. the term mode word at address 1). */
    bool canvas_first = (device == PIX_DEVICE_VGA && channel == 0 && address == 0 && count > 1);
    if (canvas_first && !pix_deliver(device, channel, address, word_at(0)))
        return api_return_errno(API_EINVAL);
    for (int i = count - 1; i >= (canvas_first ? 1 : 0); i--)
        if (!pix_deliver(device, channel, (uint8_t)(address + i), word_at(i)))
            return api_return_errno(API_EINVAL);
    return api_return_ax(0);
}
