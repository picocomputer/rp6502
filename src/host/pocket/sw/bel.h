/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_BEL_H_
#define _FPGA_SW_BEL_H_

/* bel_add and the presets are core/aud/bel.h's, so a caller rings the bell
 * the same way on every host. This is the rest of the driver. */
void bel_init(void);
void bel_task(void);

#endif /* _FPGA_SW_BEL_H_ */
