/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_AUD_H_
#define _EMU_AUD_H_

#include <stdint.h>

/* The host-facing pull/mute/viz API lives with its implementer, snd.c. */
#include "emu/aud/snd.h"

#ifdef __cplusplus
extern "C"
{
#endif

void (*aud_host_irq(void))(void); /* active sample handler, NULL before init */
uint32_t aud_host_rate(void);     /* its sample rate in Hz, 0 before init */

#ifdef __cplusplus
}
#endif

#endif /* _EMU_AUD_H_ */
