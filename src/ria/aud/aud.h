/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _AUD_H_
#define _AUD_H_

#include <stdint.h>
#include <stdbool.h>

/* Kernel events
 */

void aud_init(void);
void aud_task(void);
void aud_reclock(uint32_t sys_clk_khz);

#endif /* _AUD_H_ */
