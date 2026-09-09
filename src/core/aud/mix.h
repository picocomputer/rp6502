/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_AUD_MIX_H_
#define _CORE_AUD_MIX_H_

#include "core/aud/bel.h"
#include "core/aud/rsmp.h"
#include "core/sys/sst.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* The rate the soft machine makes samples at: a YM3812's 3579552 / 72, which
 * is 49716 exactly, and which the PSG and the bell adopted so that nothing
 * has to be converted before the mix. One build overrides it, tests/rtl/aud,
 * to hold the C at the 48 kHz its fabric model is elaborated at.
 */

#ifndef AUD_NATIVE_RATE
#define AUD_NATIVE_RATE 49716
#endif

void aud_init(void);
void aud_stop(void);

/* The device to mix, or none. psg_xreg and opl_xreg register themselves here
 * and aud_stop unregisters; the bell is not a device, every mixer adds it.
 * The engine is named rather than passed as a function pointer because a
 * savestate has to say which one is sounding in bytes another build will
 * read back.
 */

typedef enum
{
    aud_dev_none = 0,
    aud_dev_psg,
    aud_dev_opl,
} aud_dev_t;

void aud_setup(aud_dev_t dev);
aud_dev_t aud_device(void);

/* aud_device answers none while a probe is installed, so a savestate never
 * names it. */
void aud_setup_probe(void (*sample)(int16_t *left, int16_t *right));

/* Full scale of the shared sample path, sixteen bits, which is what the
 * Pocket's I2S wants. The RP2350's PWM is narrower and narrows in its own
 * mixer.
 */

#define AUD_SAMPLE_MAX 32767
#define AUD_SAMPLE_MIN (-32768)

/* Sample depth and centre of the RP2350's PWM. A wrap of 1023 at 256 MHz
 * puts the carrier at 250 kHz, and widening walks that down toward the audio
 * band, so ten bits is this chip's answer and nobody else's. Only
 * host/pico/ria/aud/mix.c may use these.
 */

#define AUD_PWM_BITS 10
#define AUD_PWM_CENTER (1u << (AUD_PWM_BITS - 1))

/* --mute: disable audio entirely — the synth never runs and the window app
 * opens no OS audio device. Default enabled. */
void aud_set_enabled(bool on);
bool aud_enabled(void);

/* What the host's converter actually runs at, once the audio backend has
 * been opened and answered. The store is a single word and aud_render
 * recomputes the resampler's step on entry, so the rate can change at any
 * time. */
void aud_set_sink_rate(uint32_t rate);

/* Fill the sink's buffer: exactly this many frames at the sink's rate, on
 * the sink's thread. Returns how many the machine made -- all of them, or 0
 * while it is muted, parked or held by a debugger. A held machine repeats
 * the level as it stands rather than dropping to silence, which would be a
 * click. */
int aud_render(float *dst, int samples);

/* Hold the sink's thread out of the engines for the length of a savestate
 * walk. aud_render runs on the sink's own thread on whatever schedule the OS
 * gives it, and a walk that read an engine mid-sample would write down a
 * machine that never existed. The host raises the request, waits for the
 * acknowledgement to catch up, walks, and drops it.
 *
 * The acknowledgement is a generation rather than a flag because the thread
 * that stores it can be preempted between reading the request and answering
 * it, and a stale store would otherwise satisfy the next wait with the last
 * one's answer. It is waited on by equality for that reason.
 *
 * The host bounds its wait, since a sink that has stopped calling back must
 * not hold a save forever. */
void aud_park_request(void);
bool aud_parked(void);
void aud_park_release(void);

/* Rolling mono downmix of what was rendered, for waveform display. */
const float *aud_viz_buffer(int *num_samples);
int aud_viz_pos(void);

/* The mix as it stands: which engine is sounding, the two resamplers whose
 * phase is continuous across renders, whatever the last input left pending,
 * and the level a held machine repeats.
 *
 * 1 device, 2 resamplers of 24 taps and a Q32 phase and a primed flag,
 * 8+8 pending of 4 with a count and an index, the held pair as two i16,
 * then the bell. */
#define AUD_RSMP_SIZE (RSMP_TAPS * 4 + 8 + 1)
#define AUD_SST_SIZE (1 + 2 * AUD_RSMP_SIZE + 16 * 4 + 2 + 4 + BEL_SST_SIZE)
void aud_sst_save(sst_cursor_t *c, unsigned flags);
bool aud_sst_load(sst_cursor_t *c, unsigned flags);

#define AUD_DRIVER DRIVER(aud_init, nul_task, nul_task, nul_run, aud_stop, nul_break, \
    nul_config, nul_config, SST(AUD_, 1, AUD_SST_SIZE, aud_sst_save, aud_sst_load))

#endif /* _CORE_AUD_MIX_H_ */
