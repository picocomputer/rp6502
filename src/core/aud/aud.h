/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_AUD_AUD_H_
#define _CORE_AUD_AUD_H_

/* This audio manager allows for multiple audio
 * devices and ensures only one is active at any time.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void aud_init(void);
void aud_stop(void);

/* Setup an audio system, tears down any previous setup.
 */

void aud_setup(void (*irq_fn)(void), uint32_t rate);

/* Per-sample stereo output level and IRQ acknowledge, called from each audio
 * driver's sample handler. This is the seam where the machine stops being
 * portable and a host's converter begins, so it is the ONLY place allowed to
 * narrow: a driver hands over a signed sample at full scale and the platform
 * decides what its hardware can carry. The firmware scales to its PWM, the
 * Pocket's fabric takes all sixteen bits to the I2S, and the emulator makes
 * a float of it.
 *
 * Signed, not centered, because a center is already a depth: the old
 * uint16_t carrying 0..1023 forced every driver to know AUD_PWM_BITS, and
 * the RP2350's ten bits then travelled to hosts with far better converters.
 */

void aud_out(int16_t left, int16_t right);
void aud_clear_irq(void);

/* The rate this host wants generated, for the drivers that get to choose.
 * The RP2350's PWM will carry any rate; the Pocket's fabric is fixed at
 * 48 kHz; the emulator answers with whatever the sound card returned. The
 * OPL2 is the exception and ignores this — a YM3812 runs at 49716 Hz or it
 * is not a YM3812 — which is the whole reason a resampler exists.
 */

uint32_t aud_native_rate(void);

/* Full scale of the shared sample path. Sixteen bits because that is what
 * the Pocket's I2S wants and what the OPL2 already produces; the RP2350's
 * PWM is the narrow one and it narrows in its own aud_out.
 */

#define AUD_SAMPLE_MAX 32767
#define AUD_SAMPLE_MIN (-32768)

/* Audio sample depth and center of the RP2350's PWM. Its wrap is 1023,
 * which puts the carrier at 250 kHz; widening walks that down toward the
 * audio band, so ten bits is this chip's answer and nobody else's. Only
 * src/core/aud/aud.c may use these.
 */

#define AUD_PWM_BITS 10
#define AUD_PWM_CENTER (1u << (AUD_PWM_BITS - 1))

/* Sine table for waveform generation, shared by all audio drivers. Sixteen
 * bits: at eight it put a -49.9 dB floor under every sine the PSG and the
 * bell can make, which no amount of width downstream could lift.
 */

extern int16_t aud_sine_table[256];

/* Build it. Every machine calls this from its own aud_init. */
void aud_sine_init(void);

#endif /* _CORE_AUD_AUD_H_ */
