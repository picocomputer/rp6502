/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Settings, as the machine sees them. Core changes a setting and says so; where
 * the bytes go is the platform's -- flash on a Pico, nowhere in the emulator,
 * the Pocket's own menu on a Pocket. */

#ifndef _CORE_CFG_H_
#define _CORE_CFG_H_

// Unconditionally save the config. A no-op where nothing persists.
void cfg_save(void);

#endif /* _CORE_CFG_H_ */
