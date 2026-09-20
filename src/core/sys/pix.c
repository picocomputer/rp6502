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

static bool pix_deliver(uint8_t dev, uint8_t channel, uint8_t address, uint16_t word);

static uint16_t pix_word_at(int i)
{
    uint16_t word;
    memcpy(&word, &xstack[XSTACK_SIZE - 5 - 2 * i], sizeof(word));
    return word;
}

/* The mode register at VGA channel 0 address 1 consumes every register above
 * it, so it is the one key register in the machine that takes arguments. */
static int pix_arg_count(uint8_t dev, uint8_t channel, uint8_t address, int remaining)
{
    if (dev == PIX_DEVICE_VGA && channel == 0 && address == 1)
        return remaining;
    return 0;
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
    /* A burst is a run of key registers, written in order. A key register that
     * takes arguments has them written first, from the highest address down,
     * because writing the key register consumes them. A key register that
     * fails aborts the burst and leaves the machine in an unknown state. */
    for (int i = 0; i < count;)
    {
        int args = pix_arg_count(device, channel, (uint8_t)(address + i), count - i - 1);
        for (int j = i + args; j > i; j--)
            if (!pix_deliver(device, channel, (uint8_t)(address + j), pix_word_at(j)))
                return api_return_errno(API_EINVAL);
        if (!pix_deliver(device, channel, (uint8_t)(address + i), pix_word_at(i)))
            return api_return_errno(API_EINVAL);
        i += args + 1;
    }
    return api_return_ax(0);
}

/* Devices 2 through 6 are open for user expansion and this machine has none,
 * so a message to one cannot fail. */
static bool pix_deliver(uint8_t dev, uint8_t channel, uint8_t address, uint16_t word)
{
    if (dev == PIX_DEVICE_RIA)
        return xreg0(channel, address, word);
    if (dev == PIX_DEVICE_VGA)
        return xreg1(channel, address, word);
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
