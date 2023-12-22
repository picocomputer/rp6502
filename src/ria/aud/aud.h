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
void aud_stop(void);
void aud_reclock(uint32_t sys_clk_khz);

/* Setup an audio system, tears down previous if any
 */

void aud_setup(
    void (*start_fn)(void),
    void (*stop_fn)(void),
    void (*reclock_fn)(uint32_t sys_clk_khz),
    void (*task_fn)(void));

#endif /* _AUD_H_ */
