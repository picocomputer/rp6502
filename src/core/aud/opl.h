/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_AUD_OPL_H_
#define _CORE_AUD_OPL_H_

/* OPL2 sound generator
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* One mono sample at AUD_NATIVE_RATE, which is a YM3812's own rate. */
int16_t opl_sample(void);

bool opl_xreg(uint16_t word);

#endif /* _CORE_AUD_OPL_H_ */
