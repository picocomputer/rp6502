/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * xreg dispatcher (xreg.c): the in-process stand-in for the PIX bus. Routes a
 * device/channel/address word to the RIA-local devices (keyboard/mouse/gamepad,
 * PSG/OPL) and the VGA canvas/mode programming.
 */

#ifndef _EMU_XREG_H_
#define _EMU_XREG_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Returns false on an unhandled device/channel (caller maps to EINVAL). */
bool emu_xreg(uint8_t device, uint8_t channel, uint8_t address, uint16_t word);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_XREG_H_ */
