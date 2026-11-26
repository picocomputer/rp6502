/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_TIM_OS_H_
#define _CORE_TIM_OS_H_

/* What core/tim/tim.h asks of a machine, for one with an operating system
 * under it: the host's clock, its zone, and the words of its locale. */

/* The locale is loaded as each program starts and let go as it stops: a
 * host that unloads this library takes the handle with it (osal os.h). */
void tim_run(void);
void tim_stop(void);

#define TIM_DRIVER DRIVER(RUN(tim_run) STOP(tim_stop))

#endif /* _CORE_TIM_OS_H_ */
