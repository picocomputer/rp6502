/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "core/pix.h"
#include "core/main.h"
#include "core/api/api.h"

/* The receiver side of the PIX bus, collapsed into the emu. On a Pico a
 * message crosses four wires to another chip; here every device the bus
 * would reach is this same binary, so delivery is a call. */

/* Deliver one PIX message. Device 0 (XRAM) is the shared xram[], already
 * satisfied, so dropped; VGA goes to the xreg dispatch; 2-7 have no emu hardware.
 * Returns the VGA's ACK/NAK (true otherwise). The RIA-local device-0 xreg is a
 * virtual pre-bus device handled in pix_api_xreg via main_xreg_0. */
bool pix_deliver(uint8_t dev, uint8_t channel, uint8_t byte, uint16_t word)
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

/* One XRAM here, so there is nothing to wait for. */
bool pix_ready(void)
{
    return true;
}

void pix_send_xram(uint16_t addr, uint8_t data)
{
    pix_deliver(PIX_DEVICE_XRAM, 0, data, addr);
}
