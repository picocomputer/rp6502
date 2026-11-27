/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_SOKOL_LINUX_PIPEWIRE_H_
#define _HOST_SOKOL_LINUX_PIPEWIRE_H_

#include <stdbool.h>
#include <stdint.h>

/* The sound through PipeWire's own API, which audio.c chooses over the Pulse
 * protocol when the Pulse server is PipeWire. render is host_audio_open's.
 * False when libpipewire will not load or the daemon will not answer, and
 * nothing is left running. */
bool pipewire_open(void (*render)(float *dst, int frames, uint32_t rate));
void pipewire_close(void);

#endif /* _HOST_SOKOL_LINUX_PIPEWIRE_H_ */
