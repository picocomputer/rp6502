/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The HID keyboard bitmap of ria/hid/kbd.c, fed by the platform's key
 * events instead of TinyUSB reports. A program maps the 256-bit bitmap
 * into XRAM through the device-0 xreg; each key event flips its bit and
 * rewrites the block. Word 0's low bits are reserved: bit 0 says no keys
 * are pressed, bits 1-3 mirror the lock LEDs — no lock handling here, so
 * they stay clear.
 */

#include "kbd.h"
#include "mmio.h"

#include "ria/sys/mem.h"

#include <string.h>

static uint32_t kbd_keys[8] = {1}; /* idle: no keys down */
static uint16_t kbd_xram = 0xFFFF; /* 0xFFFF = not mapped */

static void kbd_write_xram(void)
{
    kbd_keys[0] &= ~0xFu;
    bool any = false;
    for (int k = 0; k < 8; k++)
        if (kbd_keys[k])
            any = true;
    if (!any)
        kbd_keys[0] |= 1;
    if (kbd_xram != 0xFFFF)
        memcpy(&xram[kbd_xram], kbd_keys, sizeof(kbd_keys));
}

bool kbd_set_xram(uint16_t addr)
{
    if (addr != 0xFFFF && addr > 0x10000 - sizeof(kbd_keys))
        return false;
    kbd_xram = addr;
    kbd_write_xram();
    return true;
}

void kbd_task(void)
{
    uint32_t k = MMIO_HIDKEY;
    if (!(k & 0x200))
        return;
    uint8_t code = k & 0xFF;
    /* Keycodes 0-3 are reserved; their bits in word 0 are the flags. */
    if (code < 4)
        return;
    if (k & 0x100)
        kbd_keys[code >> 5] |= 1u << (code & 31);
    else
        kbd_keys[code >> 5] &= ~(1u << (code & 31));
    kbd_write_xram();
}

void kbd_stop(void)
{
    memset(kbd_keys, 0, sizeof(kbd_keys));
    kbd_keys[0] = 1;
    kbd_xram = 0xFFFF;
}
