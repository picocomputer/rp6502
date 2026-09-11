/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The core/hid/hid.h host functions on a machine that runs in software and has
 * no HID transport of its own. The host owns the lock keys, nothing enumerates
 * at boot, and every report the host hands over is forwarded.
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
