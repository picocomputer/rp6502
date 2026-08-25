/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What this machine does with a PIX message, which is the whole of what
 * core/pix.c leaves to it: there is no bus, one XRAM, and one of every
 * device, so the video half is a call and the rest is nowhere to go.
 */

#include "main.h"

#include "core/api/api.h"
#include "core/pix.h"

/* Devices 2-7 go over a bus this machine does not have; 0 is the shared
 * XRAM, already written. */
bool pix_deliver(uint8_t dev, uint8_t channel, uint8_t byte, uint16_t word)
{
    if (dev == PIX_DEVICE_VGA)
        return main_xreg_1(channel, byte, word);
    return true;
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
