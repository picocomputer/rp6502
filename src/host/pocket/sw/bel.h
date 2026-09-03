/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_POCKET_SW_BEL_H_
#define _HOST_POCKET_SW_BEL_H_

/* bel_add and the presets are core/aud/bel.h's, so a caller rings the bell
 * the same way on every host. This is the rest of the driver. */
void bel_init(void);
void bel_task(void);

/* This driver's row in a machine's driver list; see core/sys/driver.h. The bell's decay. Init is aud's -- the bell is part of the mixer it
 * belongs to, and is restored with it. */
#define BEL_DRIVER DRIVER(nul_init, bel_task, nul_task, nul_run, nul_stop, nul_break, nul_config, nul_config)

#endif /* _HOST_POCKET_SW_BEL_H_ */
