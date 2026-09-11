/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Op 0x01 on a machine whose PIX devices are all itself. pix_api_xreg calls
 * pix_deliver instead of putting a message on a bus, and the false a handler
 * returns becomes API_EINVAL. A machine with a real bus implements the op in
 * host/pico/ria/sys/pix.c and links none of this.
 */

#include "core/api/xreg.h"
#include "core/sys/pix.h"
#include "core/sys/driver.h"
#include "core/api/api.h"
#include "core/ria/regs.h"
#include "core/sys/xram.h"
#include <string.h>

static bool pix_deliver(uint8_t dev, uint8_t channel, uint8_t byte, uint16_t word);

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
    xstack_ptr = XSTACK_SIZE;
    if (!aligned || count < 1 || count > XSTACK_SIZE / 2 ||
        device > 7 || channel > 15)
        return api_return_errno(API_EINVAL);
    /* Channel $F is the VGA control channel, which core/api/xreg1.c answers
     * for the machine itself, so a write by a program is refused. The RIA
     * refuses it only while a VGA is connected; this machine is its own. */
    if (device == PIX_DEVICE_VGA && channel == 0xF)
        return api_return_errno(API_EACCES);
    if (device == PIX_DEVICE_RIA)
    {
        for (int i = count - 1; i >= 0; i--)
            if (!xreg0(channel, address, pix_word_at(i)))
                return api_return_errno(API_EINVAL);
        return api_return_ax(0);
    }
    /* A canvas write clears the mode programming that follows it, so a burst
     * starting at VGA channel 0 address 0 delivers the canvas first. The rest
     * go from the highest address down, because the mode write at address 1
     * consumes the parameter registers above it. */
    bool canvas_first = (device == PIX_DEVICE_VGA && channel == 0 && address == 0 && count > 1);
    if (canvas_first && !pix_deliver(device, channel, address, pix_word_at(0)))
        return api_return_errno(API_EINVAL);
    for (int i = count - 1; i >= (canvas_first ? 1 : 0); i--)
        if (!pix_deliver(device, channel, (uint8_t)(address + i), pix_word_at(i)))
            return api_return_errno(API_EINVAL);
    return api_return_ax(0);
}

/* Nothing but VGA channel 0 registers 0 and 1 is acknowledged even where a
 * bus exists, so a message to the rest cannot fail. */
static bool pix_deliver(uint8_t dev, uint8_t channel, uint8_t byte, uint16_t word)
{
    if (dev == PIX_DEVICE_VGA)
        return xreg1(channel, byte, word);
    return true;
}

/* There is no FIFO to fill, and std_task drains its forwarding count only
 * while this is true, so a false would stall it forever. */
bool pix_ready(void)
{
    return true;
}

/* std_task forwards a byte it read out of xram, and this machine has the one
 * copy, so the byte is already where it was going. */
void pix_send_xram(uint16_t addr, uint8_t data)
{
    (void)addr;
    (void)data;
}
