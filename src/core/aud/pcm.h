/*
 * Copyright (c) 2026 WojciechGw
 * 
 * for Rumbledethumps' Picocomputer 6502
 * 
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_AUD_PCM_H_
#define _CORE_AUD_PCM_H_

/* PCM audio playback - stereo ring buffer in XRAM, resampled from a
 * program's chosen rate up to AUD_NATIVE_RATE. It is not an aud_dev: every
 * mixer adds it to the PSG or the OPL, and 0xFFFF in its xreg stops it.
 */

#include "core/sys/sst.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* One stereo sample at AUD_NATIVE_RATE, and the engine advanced by one.
 * Silence while stopped. */
void pcm_sample(int16_t *left, int16_t *right);

bool pcm_xreg(uint16_t word);

/* The caller holds the engine lock, or nothing samples concurrently. */
void pcm_stop(void);

/* base, mask, format, frame_sz, read_ptr, l0/r0/l1/r1, phase, phase_inc. */
#define PCM_SST_SIZE (2 + 2 + 1 + 1 + 2 + 4 * 2 + 4 + 4)
void pcm_sst_save(sst_cursor_t *c, unsigned flags);
bool pcm_sst_load(sst_cursor_t *c, unsigned flags);

#define PCM_DRIVER DRIVER(nul_init, nul_task, nul_task, nul_run, nul_stop, nul_break, \
    nul_config, nul_config, SST(PCM_, 1, PCM_SST_SIZE, pcm_sst_save, pcm_sst_load))

#endif /* _CORE_AUD_PCM_H_ */
