/*
 * Copyright (c) 2026 WojciechGw
 * 
 * for Rumbledethumps' Picocomputer 6502
 * 
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_AUD_PCM_H_
#define _RIA_AUD_PCM_H_

/* PCM audio playback - 16-bit stereo 44100 Hz ring buffer in XRAM.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

bool pcm_xreg(uint16_t word);

#endif /* _RIA_AUD_PCM_H_ */
