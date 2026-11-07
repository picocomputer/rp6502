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
} bel_sound_t;

/* The bell's sequencer. Its voice is the PSG's ninth, which every machine
 * reaches through aud_bel_put. Its deadlines are host_clock_us, polled by
 * bel_task. */
void bel_init(void);
void bel_task(void);
void bel_add(const bel_sound_t *sound);

/* Moves the deadlines by us microseconds, for a machine whose clock ran
 * on while the bell's did not. */
void bel_wake(uint64_t us);

/* aud_sst_save writes this inside the AUD chunk; the bell has no row of
 * its own.
 *
 * The count of queued sounds, 8 sounds of 12 oldest first, then the
 * restrike, release and end deadlines of 8. */
#define BEL_SST_SIZE (1 + 8 * 12 + 3 * 8)
void bel_sst_save(sst_cursor_t *c, unsigned flags);
bool bel_sst_load(sst_cursor_t *c, unsigned flags);

extern const bel_sound_t bel_teletype;
extern const bel_sound_t bel_nfc_fail;
extern const bel_sound_t bel_nfc_success_1;
extern const bel_sound_t bel_nfc_success_2;

#endif /* _CORE_AUD_BEL_H_ */
