/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_AUD_OPL_H_
#define _RIA_AUD_OPL_H_

/* Yamaha OPL sound generator
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

bool opl_xreg(uint16_t word);

#endif /* _RIA_AUD_OPL_H_ */
