/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_HOST_TIME_H_
#define _EMU_HOST_TIME_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* The deterministic virtual master clock, in 1/8-of-a-256MHz-tick units (2048
 * per microsecond, held that fine so the PHI2 fractional divider lands on an
 * integer per-cycle step). The 6502 advances it as it ticks; time_us_64()
 * (the pico/time.h shim) exposes it as the monotonic microsecond clock. */
uint64_t time_clock_8(void);         /* the current master clock */
void time_advance_8(uint32_t ticks); /* add one 6502 cycle's worth */
void time_set_8(uint64_t ticks);     /* jump the clock (the halt clamp keeps time flowing) */
void time_reset(void);               /* zero at cold boot */

#ifdef __cplusplus
}
#endif

#endif /* _EMU_HOST_TIME_H_ */
