/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_AUD_BEL_H_
#define _CORE_AUD_BEL_H_

/* Bell/alert audio device - single channel mono synth.
 * Always available: every mixer adds it to whatever else is sounding.
 */

#include "core/sys/sst.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Sound descriptor for queuing bell sounds.
 */

typedef struct
{
    // First five same as PSG
    uint16_t freq;
    uint8_t duty;
    uint8_t vol_attack;
    uint8_t vol_decay;
    uint8_t wave_release;
    // Queue timing options
    uint16_t restrike_ms;
    uint16_t release_ms;
    uint16_t end_ms;
} ria_bel_t;

/* Seed the generator. Once, from a machine's aud_init. */
void bel_init(void);

/* One mono sample at AUD_NATIVE_RATE; zero while nothing is rung. */
int16_t bel_sample(void);

// Queue a sound to play.
void bel_add(const ria_bel_t *sound);

/* The bell rides the AUD chunk rather than a row of its own: aud_init is
 * what starts it, and the name BEL is the Pocket's own driver.
 *
 * 8 queued sounds of 12, a head and a tail, then the generator: 2 sample,
 * 1 adsr, 4 vol, 4 phase, 4+4 noise, 4 elapsed, 1 active. */
#define BEL_SST_SIZE (8 * 12 + 2 + 24)
void bel_sst_save(sst_cursor_t *c, unsigned flags);
bool bel_sst_load(sst_cursor_t *c, unsigned flags);

// Preset bell sounds
extern const ria_bel_t bel_teletype;
extern const ria_bel_t bel_nfc_fail;
extern const ria_bel_t bel_nfc_success_1;
extern const ria_bel_t bel_nfc_success_2;

#endif /* _CORE_AUD_BEL_H_ */
