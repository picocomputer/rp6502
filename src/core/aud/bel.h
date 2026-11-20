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

/* The queue and the deadlines. The bell's voice is the PSG's, and its row
 * carries it. aud_init starts the bell, so this driver has no INIT or
 * RESET. */
extern const sst_table_t bel_sst;

#define BEL_DRIVER DRIVER(                                  \
    SST("BEL_", bel))

extern const bel_sound_t bel_teletype;
extern const bel_sound_t bel_nfc_fail;
extern const bel_sound_t bel_nfc_success_1;
extern const bel_sound_t bel_nfc_success_2;

#endif /* _CORE_AUD_BEL_H_ */
