/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Host audio output (audio_out.c): drain the emulator's native-rate sample ring,
 * linear-resample it to the sokol-audio device rate, and push. Lives in the app
 * (window) target because it touches sokol-audio; the synth itself is snd.c/aud.c
 * in emu_core. Called once per frame after the machine has run.
 */

#ifndef _EMU_AUDIO_OUT_H_
#define _EMU_AUDIO_OUT_H_

#ifdef __cplusplus
extern "C"
{
#endif

/* Drain this frame's native-rate samples, resample to the host device rate, and
 * push them to sokol-audio. A no-op when built without audio. */
void audio_out_pump(void);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_AUDIO_OUT_H_ */
