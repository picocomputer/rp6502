/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
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
