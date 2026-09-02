/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _CORE_AUD_AUD_MIX_H_
#define _CORE_AUD_AUD_MIX_H_

#include <stdbool.h>

#include "core/aud/aud.h"

/* Main events
 */

/* --mute: disable audio entirely — the synth stops running (no per-frame
 * CPU work) and the window app opens no OS audio device. Default enabled. */
void aud_set_enabled(bool on);
bool aud_enabled(void);

/* Native sample rate (Hz) of the active device, or 0 when silent / disabled. */
int aud_rate(void);

/* Tell the machine what the host's converter actually runs at, once the
 * audio backend has been opened and answered. Everything whose rate is ours
 * to choose is then generated at it and needs no conversion at all; only the
 * OPL2 is resampled, because a YM3812 runs at 49716 Hz or it is not one.
 * Call before the machine starts: it re-derives the PSG's tables and
 * re-registers the standing bell, so it resets the audio devices. */
void aud_set_native_rate(uint32_t rate);

/* Fill the device's buffer from the registers as they stand, one handler
 * call per stereo sample, at aud_native_rate(): the PWM interrupt, on the
 * device's thread. Silence when muted or no device is installed. */
void aud_render(float *dst, int samples);

/* Rolling mono downmix of the produced output, for waveform display. */
const float *aud_viz_buffer(int *num_samples);
int aud_viz_pos(void); /* current write position in that buffer */

#endif /* _CORE_AUD_AUD_MIX_H_ */
