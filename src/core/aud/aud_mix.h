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

/* Make the samples the machine's clock has reached, one handler call each
 * at the device's rate, on the machine's thread: the PWM interrupt. A pass
 * is a scanline, so a program's register writes drain as it makes them
 * instead of waiting in the queue for the device to ask. */
void aud_task(void);

/* The machine may lead the device by this many frames; past that what it
 * makes is dropped, so a stall's burst of frames is not kept as latency. */
#define AUD_LEAD_FRAMES 3

/* Fill the device's buffer from what the machine has made, at
 * aud_native_rate(), on the device's thread. Returns how many samples the
 * machine had; the rest repeat the level as it stands -- never silence,
 * which is a click. The machine must lead by a frame and this request
 * before it is served at all, or a request between two frames is short.
 * Silence when muted. */
int aud_render(float *dst, int samples);

/* Rolling mono downmix of the produced output, for waveform display. */
const float *aud_viz_buffer(int *num_samples);
int aud_viz_pos(void); /* current write position in that buffer */

/* This driver's row in a machine's driver list; see core/driver.h. Over
 * core/aud/aud.h's, which is the Pico's: this machine makes its samples in
 * a task. */
#undef AUD_DRIVER
#define AUD_DRIVER DRIVER(aud_init, aud_task, nul_task, nul_run, aud_stop, nul_break, nul_config, nul_config)

#endif /* _CORE_AUD_AUD_MIX_H_ */
