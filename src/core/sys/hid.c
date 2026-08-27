/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * core/hid/hid.h's host hooks, answered by a machine with no transport of
 * its own: the lock keys belong to the desktop's own keyboard, there is
 * nothing to enumerate at boot, and a remapped device is refilled by the
 * next thing the window hands us.
 *
 * Beside the transport rather than beside a driver table, which is where the
 * other two machines put them -- host/pico/ria/usb/usb.c and
 * host/pocket/sw/hid.c both answer these next to the bus they speak for.
 */

#include "core/hid/hid.h"

void hid_set_leds(uint8_t leds)
{
    (void)leds;
}

bool hid_boot_enumerating(void)
{
    return false;
}

void hid_remapped(void)
{
}
