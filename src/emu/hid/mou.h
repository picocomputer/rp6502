/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Mouse input (mou.c) — replaces the firmware USB HID mouse driver. Points a
 * report block ({buttons,x,y,wheel,pan}, 5 bytes) at XRAM (xreg_ria_mouse) and
 * accumulates host pointer motion/buttons into it (ria/hid/mou.c).
 */

#ifndef _EMU_MOU_H_
#define _EMU_MOU_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* xreg_ria_mouse API: point the report block ({buttons,x,y,wheel,pan}, 5 bytes)
 * at an XRAM address (0xFFFF = off). Mirrors ria/hid/mou.c mou_xreg. */
bool mou_set_xram(uint16_t addr);

/* True once a program has pointed the report block at XRAM (xreg_ria_mouse).
 * The window layer gates mouse capture on this. */
bool mou_is_mapped(void);

/* Accumulate host pointer motion. dx/dy are in mouse-counter units (the window
 * layer converts window-pixel motion to canvas space so the speed is
 * independent of window scaling). Fractional motion is carried between calls. */
void mou_host_move(float dx, float dy);

/* Set the button byte (bit 0 left, 1 right, 2 middle), mirroring the HID order. */
void mou_host_buttons(uint8_t buttons);

void mou_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_MOU_H_ */
