/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The PIX bus, for a machine whose PIX devices are all itself. On a Pico a
 * message crosses four wires to another chip and the answer comes back
 * later; here every device the bus would reach is the same binary, so a
 * delivery is a call and a handler's false is the NAK.
 *
 * Two such machines exist and they were answering identically -- the video
 * half is main_xreg_1 on both, and both have one XRAM that the write has
 * already reached -- so there is nothing left below this to be per-machine.
 * A machine with a real bus (host/pico/ria/sys/pix.c) implements op 0x01
 * itself and links none of this.
 */

#include "core/pix.h"
#include "core/main.h"
#include "core/api/api.h"
#include "core/mem.h"
#include <string.h>

static bool pix_deliver(uint8_t dev, uint8_t channel, uint8_t byte, uint16_t word);

/* The i-th xreg data word (target address+i) sits at xstack[SIZE-5-2i]. */
static uint16_t pix_word_at(int i)
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
     * on a machine that is its own VGA), so a write NAKs. */
    if (device == PIX_DEVICE_VGA && channel == 0xF)
        return api_return_errno(API_EACCES);
    /* Device 0 is the RIA-local virtual xreg, never bussed: dispatch straight to
     * main_xreg_0 with the address held constant (last-wins). */
    if (device == PIX_DEVICE_RIA)
    {
        for (int i = count - 1; i >= 0; i--)
            if (!main_xreg_0(channel, address, pix_word_at(i)))
                return api_return_errno(API_EINVAL);
        return api_return_ax(0);
    }
    /* A VGA channel-0 write from address 0 must send the canvas word (address 0)
     * first so it can't clear later mode programming; the rest follow high
     * address -> low, landing each register after the parameters it consumes
     * (e.g. the term mode word at address 1). */
    bool canvas_first = (device == PIX_DEVICE_VGA && channel == 0 && address == 0 && count > 1);
    if (canvas_first && !pix_deliver(device, channel, address, pix_word_at(0)))
        return api_return_errno(API_EINVAL);
    for (int i = count - 1; i >= (canvas_first ? 1 : 0); i--)
        if (!pix_deliver(device, channel, (uint8_t)(address + i), pix_word_at(i)))
            return api_return_errno(API_EINVAL);
    return api_return_ax(0);
}

/* Where a message goes on a machine whose devices are all itself. Device 0 is
 * the shared XRAM, already written; the video half is a call; and 2-7 would
 * have gone over a bus that is not there. */
static bool pix_deliver(uint8_t dev, uint8_t channel, uint8_t byte, uint16_t word)
{
    if (dev == PIX_DEVICE_VGA)
        return main_xreg_1(channel, byte, word);
    return true;
}

/* One XRAM, so there is no bus to fill. Always ready: std_task retires its
 * forwarding count through this, and a false would park the drain. */
bool pix_ready(void)
{
    return true;
}

/* One XRAM, and the write that got here already landed in it. */
void pix_send_xram(uint16_t addr, uint8_t data)
{
    (void)addr;
    (void)data;
}
