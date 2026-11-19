/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_TIM_OS_H_
#define _CORE_TIM_OS_H_

/* What core/tim/tim.h asks of a machine, for one with an operating system
 * under it: the host's clock, its zone, and the words of its locale. */

void tim_init(void);
void tim_stop(void);

#define TIM_DRIVER DRIVER(INIT(tim_init) STOP(tim_stop))

#endif /* _CORE_TIM_OS_H_ */
