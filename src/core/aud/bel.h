/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_AUD_BEL_H_
#define _CORE_AUD_BEL_H_

#include "core/sys/sst.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct
{
    // The first five fields are a PSG channel's first five.
    uint16_t freq;
    uint8_t duty;
    uint8_t vol_attack;
    uint8_t vol_decay;
    uint8_t wave_release;
    uint16_t restrike_ms;
    uint16_t release_ms;
    uint16_t end_ms;
} ria_bel_t;

void bel_init(void);

/* One sample, zero while nothing is rung. The phase and envelope steps are
 * derived from AUD_NATIVE_RATE, so a mixer has to call this once per sample
 * at that rate. */
int16_t bel_sample(void);

void bel_add(const ria_bel_t *sound);

/* aud_sst_save writes this inside the AUD chunk; the bell has no row of
 * its own.
 *
 * 8 queued sounds of 12, a head and a tail, then the generator: 2 sample,
 * 1 adsr, 4 vol, 4 phase, 4+4 noise, 4 elapsed, 1 active. */
#define BEL_SST_SIZE (8 * 12 + 2 + 24)
void bel_sst_save(sst_cursor_t *c, unsigned flags);
bool bel_sst_load(sst_cursor_t *c, unsigned flags);

extern const ria_bel_t bel_teletype;
extern const ria_bel_t bel_nfc_fail;
extern const ria_bel_t bel_nfc_success_1;
extern const ria_bel_t bel_nfc_success_2;

#endif /* _CORE_AUD_BEL_H_ */
