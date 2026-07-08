/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_SND_H_
#define _EMU_SND_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

void snd_task(void);  /* generate this frame's samples from the active device */
void snd_reset(void); /* silence + clear the ring (machine reset / exec) */

/* Native sample rate (Hz) of the active device, or 0 when silent / disabled. */
int emu_audio_rate(void);

/* --mute: disable audio entirely — the synth stops running (no per-frame
 * CPU work) and the window app opens no OS audio device. Default enabled. */
void emu_set_audio_enabled(bool on);
bool emu_audio_enabled(void);

/* Pull up to max_frames interleaved stereo frames (L,R floats in [-1,1]) from
 * the native-rate ring. Returns the number of frames written. */
int emu_audio_read(float *dst, int max_frames);

/* Rolling mono downmix of the produced output, for waveform display. */
const float *emu_audio_viz_buffer(int *num_samples);
int emu_audio_viz_pos(void); /* current write position in that buffer */

#ifdef __cplusplus
}
#endif

#endif /* _EMU_SND_H_ */
