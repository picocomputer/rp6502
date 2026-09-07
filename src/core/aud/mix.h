/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_AUD_MIX_H_
#define _CORE_AUD_MIX_H_

/* The mixer's contract, which every machine with the C engines answers one
 * of: core/aud/mix.c mixes for a host's sink, host/pico/ria/aud/mix.c for a
 * PWM. Each registers one device at a time and adds the bell to whatever it
 * is.
 */

#include "core/aud/bel.h"
#include "core/aud/rsmp.h"
#include "core/sys/sst.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* The rate the soft machine makes samples at, and the only one: a YM3812's,
 * 3579552 / 72, which the PSG and the bell adopted so that nothing has to be
 * converted before the mix. A constant, so every divisor and envelope step
 * derived from it folds. The Pico's PWM runs at it; the emulator resamples
 * from it to whatever the sink wants. Overridden by exactly one build, the
 * PSG lockstep, whose fabric model is elaborated at 48 kHz.
 */

#ifndef AUD_NATIVE_RATE
#define AUD_NATIVE_RATE 49716
#endif

/* Main events
 */

void aud_init(void);
void aud_stop(void);

/* The device to mix, or none. A mixer calls it once per sample for a stereo
 * pair at AUD_NATIVE_RATE. psg_xreg and opl_xreg register themselves here and
 * aud_stop unregisters; the bell is not a device, every mixer adds it.
 *
 * Named rather than passed: a function address is this build's own, and a
 * savestate has to say which engine is sounding in bytes another build will
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

/* A device that is neither engine, for a test that counts what the mixer
 * asks for. Never on the wire -- a blob names one of the machine's own two
 * -- and aud_device answers none while one is installed. */
void aud_setup_probe(void (*sample)(int16_t *left, int16_t *right));

/* Full scale of the shared sample path. Sixteen bits because that is what
 * the Pocket's I2S wants and what the OPL2 already produces; the RP2350's
 * PWM is the narrow one and it narrows in its own mixer.
 */

#define AUD_SAMPLE_MAX 32767
#define AUD_SAMPLE_MIN (-32768)

/* Audio sample depth and center of the RP2350's PWM. Its wrap is 1023,
 * which puts the carrier at 250 kHz; widening walks that down toward the
 * audio band, so ten bits is this chip's answer and nobody else's. Only
 * host/pico/ria/aud/mix.c may use these.
 */

#define AUD_PWM_BITS 10
#define AUD_PWM_CENTER (1u << (AUD_PWM_BITS - 1))

/* ---- what only the soft mixer, core/aud/mix.c, answers ---- */

/* --mute: disable audio entirely — the synth never runs and the window app
 * opens no OS audio device. Default enabled. */
void aud_set_enabled(bool on);
bool aud_enabled(void);

/* What the host's converter actually runs at, once the audio backend has
 * been opened and answered. One store: the resampler's step comes out of it
 * on the next render, so it can change at any time. */
void aud_set_sink_rate(uint32_t rate);

/* Fill the sink's buffer: exactly this many frames at the sink's rate, on
 * the sink's thread. Returns how many the machine made -- all of them, or 0
 * while a debugger holds it, when every frame repeats the level as it stands
 * rather than dropping to silence, which is a click. Silence when muted. */
int aud_render(float *dst, int samples);

/* Hold the sink's thread out of the engines for the length of a savestate
 * walk. aud_render runs on the sink's own thread on whatever schedule the
 * OS gives it, and a walk that read an engine mid-sample would write down a
 * machine that never existed. The host raises the request, waits for the
 * acknowledgement to catch up, walks, and drops it.
 *
 * A generation rather than a flag, because the acknowledgement is stored by
 * a thread that may be preempted between reading the request and answering
 * it: a stale store would otherwise satisfy the next wait with the last
 * one's answer. Waited on by equality for that reason.
 *
 * The acknowledgement is stored ahead of the mute test, not behind it. A
 * muted device never renders anything, and an acknowledgement behind that
 * test would leave every wait running to its timeout.
 *
 * The host waits only when it has a device open and the render is not on
 * its own thread, and it bounds the wait: a sink that has stopped calling
 * back must not hold a save forever. */
void aud_park_request(void);
bool aud_parked(void);
void aud_park_release(void);

/* Rolling mono downmix of what was rendered, for waveform display. */
const float *aud_viz_buffer(int *num_samples);
int aud_viz_pos(void); /* current write position in that buffer */

/* The mix as it stands: which engine is sounding, the two resamplers whose
 * phase is continuous across renders, whatever the last input left pending,
 * and the level a held machine repeats. The bell rides here too, because
 * aud_init is what starts it and the Pocket owns the name BEL.
 *
 * Carried exactly rather than restarted. Rewind serializes every frame and
 * an engine that began again on each load would click sixty times a second.
 *
 * 1 device, 2 resamplers of 24 taps and a Q32 phase and a primed flag,
 * 8+8 pending of 4 with a count and an index, the held pair as two i16,
 * then the bell. */
#define AUD_RSMP_SIZE (RSMP_TAPS * 4 + 8 + 1)
#define AUD_SST_SIZE (1 + 2 * AUD_RSMP_SIZE + 16 * 4 + 2 + 4 + BEL_SST_SIZE)
void aud_sst_save(sst_cursor_t *c, unsigned flags);
bool aud_sst_load(sst_cursor_t *c, unsigned flags);

/* This driver's row in a machine's driver list; see core/sys/driver.h. */
#define AUD_DRIVER DRIVER(aud_init, nul_task, nul_task, nul_run, aud_stop, nul_break, \
    nul_config, nul_config, SST(AUD_, 1, AUD_SST_SIZE, aud_sst_save, aud_sst_load))

#endif /* _CORE_AUD_MIX_H_ */
