/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * core/hid/hid.h's two host hooks, answered by a machine with no
 * transport of its own: the lock keys belong to the desktop's own
 * keyboard, and there is nothing to enumerate at boot.
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
