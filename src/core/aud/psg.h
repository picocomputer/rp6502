/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_AUD_PSG_H_
#define _CORE_AUD_PSG_H_

/* Programmable Sound Generator
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

/* Choose the rate the engine generates at. The platform's aud_init calls
 * this before anything can ask for the device, because the three hosts do
 * not agree: the RP2350's PWM will carry any rate, the Pocket's fabric is
 * fixed at 48 kHz, and the emulator takes whatever the sound card returned.
 * Everything rate-dependent — both envelope tables and the divisor the phase
 * increments come out of — is derived here, off the sample path.
 */

void psg_setup(uint32_t rate);

bool psg_xreg(uint16_t word);

#endif /* _CORE_AUD_PSG_H_ */
