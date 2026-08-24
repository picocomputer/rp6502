/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The pocket port of the op 0x01 xreg dispatcher, emu/sys/pix.c's
 * synchronous shape: no PIX bus, so delivery is a function call and a
 * handler's false is the NAK. The canvas word goes first from a
 * channel-0 address-0 burst so it cannot clear the mode programming that
 * follows; the rest land high address to low, each register after the
 * parameters it consumes.
 */

#include "main.h"

#include "core/api/api.h"
#include "core/pix.h"

#include <string.h>

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
    xstack_ptr = XSTACK_SIZE;
    if (!aligned || count < 1 || count > XSTACK_SIZE / 2 ||
        device > 7 || channel > 15)
        return api_return_errno(API_EINVAL);
    /* The VGA control channel is RIA-private. */
    if (device == PIX_DEVICE_VGA && channel == 0xF)
        return api_return_errno(API_EACCES);
    if (device == PIX_DEVICE_RIA)
    {
        for (int i = count - 1; i >= 0; i--)
            if (!main_xreg_0(channel, address, pix_word_at(i)))
                return api_return_errno(API_EINVAL);
        return api_return_ax(0);
    }
    if (device == PIX_DEVICE_VGA)
    {
        bool canvas_first = channel == 0 && address == 0 && count > 1;
        if (canvas_first && !main_xreg_1(channel, address, pix_word_at(0)))
            return api_return_errno(API_EINVAL);
        for (int i = count - 1; i >= (canvas_first ? 1 : 0); i--)
            if (!main_xreg_1(channel, (uint8_t)(address + i), pix_word_at(i)))
                return api_return_errno(API_EINVAL);
        return api_return_ax(0);
    }
    /* Devices 2-7 go over a bus this machine does not have. */
    return api_return_ax(0);
}

/* One XRAM, written directly, so there is no bus to fill. Ready is always
 * true: std_task retires its forwarding count through it, and a false here
 * would park the drain. */
bool pix_ready(void)
{
    return true;
}

void pix_send_xram(uint16_t addr, uint8_t data)
{
    (void)addr;
    (void)data;
}
