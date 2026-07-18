/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_AUD_AUD_H_
#define _EMU_AUD_AUD_H_

#include <stdbool.h>

/* Main events
 */

void aud_task(void); /* generate this frame's samples from the active device */

/* --mute: disable audio entirely — the synth stops running (no per-frame
 * CPU work) and the window app opens no OS audio device. Default enabled. */
void aud_set_enabled(bool on);
bool aud_enabled(void);

/* Native sample rate (Hz) of the active device, or 0 when silent / disabled. */
int aud_rate(void);

/* Pull up to max_frames interleaved stereo frames (L,R floats in [-1,1]) from
 * the native-rate ring. Returns the number of frames written. */
int aud_read(float *dst, int max_frames);

/* Rolling mono downmix of the produced output, for waveform display. */
const float *aud_viz_buffer(int *num_samples);
int aud_viz_pos(void); /* current write position in that buffer */

#endif /* _EMU_AUD_AUD_H_ */
