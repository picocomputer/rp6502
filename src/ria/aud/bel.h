/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_AUD_BEL_H_
#define _RIA_AUD_BEL_H_

/* Bell audio device - single channel mono synth.
 * Always active as the default audio device.
 * PSG and OPL mix in bel samples via bel_sample().
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Sound descriptor for queuing bell sounds.
 */

typedef struct
{
    uint16_t freq;
    uint8_t duty;
    uint8_t vol_attack;
    uint8_t vol_decay;
    uint8_t wave_release;
    uint16_t restrike_ms;
    uint16_t release_ms;
    uint16_t end_ms;
} ria_bel_t;

// Install self.
void bel_setup(void);

// Generate one mono sample at the given sample rate.
// Called from IRQ context (BEL, PSG, or OPL handler).
int16_t bel_sample(uint32_t rate);

// Queue a sound to play.
void aud_bel_add(const ria_bel_t *sound);

// Preset bell sounds
extern const ria_bel_t bel_typewriter;
extern const ria_bel_t bel_teletype;
extern const ria_bel_t bel_chime;

#endif /* _RIA_AUD_BEL_H_ */
