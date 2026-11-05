/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_AUD_MIX_H_
#define _CORE_AUD_MIX_H_

#include "core/aud/bel.h"
#include "core/aud/pcm.h"
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

/* Park every engine and reset the OPL2. For a host that unloads this library;
 * a host that exits needs nothing. */
void aud_shutdown(void);

/* An engine sounds while its pointer is mapped, as psg.sv and opl.sv do, so
 * the PSG and the OPL2 can sound at once and their outputs sum. The bell is
 * not an engine and every mixer adds it. aud_stop parks both engines. */

/* While a probe is installed it is the only engine mixed. */
void aud_setup_probe(void (*sample)(int16_t *left, int16_t *right));

/* Every 6502 write to XRAM, handed to the engine mapped on its page, if one
 * is. A software machine calls it where the write happens, before XRAM takes
 * the byte. It stamps the write with the sample its cycle falls in and the
 * mix applies it before making that sample, whenever the sink asks for it.
 * The Pico's RW engine is core1, which queues every write for the audio
 * interrupt on core0 instead. */
void aud_xram_write(uint16_t addr, uint8_t val);

/* An engine's xreg mapping it at word or parking it, 0xFFFF, and resetting
 * it either way. The Pico does it at once under the engine lock; a software
 * machine stamps it as it stamps a write. */
void aud_psg_map(uint16_t word);
void aud_opl_map(uint16_t word);

/* The PCM: device's sound, which each machine's mixer keeps for
 * core/aud/pcm.c. aud_pcm_open fails while one is open. aud_pcm_level is the
 * bytes waiting to play as the program sees them, at the machine's present,
 * and aud_pcm_clear empties them. */
bool aud_pcm_open(const pcm_fmt_t *fmt);
void aud_pcm_close(void);
uint32_t aud_pcm_write(const uint8_t *buf, uint32_t n);
uint16_t aud_pcm_level(void);
void aud_pcm_clear(void);

/* A software machine's present, at the end of each frame: the stamps before
 * it are all in. A sink on the machine's thread that did not take the frame
 * before has it dropped, unless a debugger holds the machine, and a muted
 * mix is stepped up to here. */
void aud_step(void);

/* Native samples the mix has made; while one is being made, its index. */
uint64_t aud_samples(void);

/* The phase increment of a frequency register that holds hertz times three,
 * as the PSG and the bell keep theirs: freq * 2^32 / (3 * AUD_NATIVE_RATE),
 * floored. The divide is done as a multiply by the reciprocal rounded up,
 * which gives the same result for every 16-bit freq at both rates this is
 * built for, 49716 Hz and the 48000 Hz of tests/rtl/aud. */
#define AUD_PHASE_MUL ((((uint64_t)1 << 63) + 3u * AUD_NATIVE_RATE - 1) / (3u * AUD_NATIVE_RATE))
static inline uint32_t aud_phase_inc(uint16_t freq)
{
    return (uint32_t)(((uint64_t)freq * AUD_PHASE_MUL) >> 31);
}

/* Held across anything that reaches into the engines from outside the mix.
 * The Pico holds its audio interrupt off; a software machine holds off the
 * sink's render. */
void aud_engine_lock(void);
void aud_engine_unlock(void);

/* OPL2 writes dropped since the last call, saturating at 0x7FFFFFFF: the
 * writes a program made faster than the queue's pace let it hold. A map,
 * an unmap and aud_stop clear it. A software machine decides each write as
 * the 6502 makes it, so nothing the host does adds one. The Pocket has no
 * queue and returns 0. */
uint32_t aud_opl_drops(void);

/* Full scale of the shared sample path, sixteen bits, which is what the
 * Pocket's I2S wants. The RP2350's PWM is narrower and narrows in its own
 * mixer.
 */

#define AUD_SAMPLE_MAX 32767
#define AUD_SAMPLE_MIN (-32768)

/* --mute: nothing is heard and the window app opens no OS audio device. The
 * engines still step with the machine; their samples are not resampled. Default
 * enabled. */
void aud_set_enabled(bool on);
bool aud_enabled(void);

/* What the host's converter actually runs at, once the audio backend has
 * been opened and answered. aud_render reads it for each run of samples, so
 * the rate can change at any time. */
void aud_set_sink_rate(uint32_t rate);

/* A sink that pulls on its own clock, such as an audio device's callback,
 * on its own thread. Its render makes what it asks for on the spot, and the
 * resampler's ratio is trimmed so that what it asks for has just been
 * stamped. Off, the default, is a sink that pulls once a frame on the
 * machine's thread. */
void aud_set_streaming(bool on);

/* Fills the sink's buffer with this many frames at the sink's rate, made on
 * the spot. A streaming sink gets them all. A sink that pulls once a frame
 * gets the samples up to the machine's present and the count is returned,
 * the newest kept if there are more; the rest repeat the level as it stands,
 * since dropping to silence would click. Muted, it is silence; held by a
 * debugger, the level repeats. Both return 0 then. */
int aud_render(float *dst, int samples);

/* Rolling mono downmix of what was rendered, for waveform display. */
const float *aud_viz_buffer(int *num_samples);
int aud_viz_pos(void);

/* The mix as it stands: which engines are sounding, the two resamplers whose
 * phase is continuous across runs, and the native samples made. It is saved
 * with the sink's sound taken, so no stamped change is still waiting.
 *
 * The engines are one byte, bit 0 the PSG and bit 1 the OPL2.
 *
 * 1 engines, 2 resamplers of 24 taps and a Q32 phase and a primed flag,
 * 8 samples made, 4 OPL2 drops, the bell, then the PCM: device: open, its
 * rate, bits and channels, the Q32 phase, both channels' 24 samples of
 * history, the bytes buffered and PCM_BUF_SIZE of them. */
#define AUD_RSMP_SIZE (RSMP_TAPS * 4 + 8 + 1)
#define AUD_PCM_SST_SIZE (1 + 4 + 1 + 1 + 8 + 2 * RSMP_TAPS * 4 + 2 + PCM_BUF_SIZE)
#define AUD_SST_SIZE (1 + 2 * AUD_RSMP_SIZE + 8 + 4 + BEL_SST_SIZE + AUD_PCM_SST_SIZE)
void aud_sst_save(sst_cursor_t *c, unsigned flags);
bool aud_sst_load(sst_cursor_t *c, unsigned flags);

#define AUD_DRIVER DRIVER(aud_init, nul_task, nul_task, nul_run, aud_stop, nul_break, \
    nul_config, nul_config, SST(AUD_, 3, AUD_SST_SIZE, aud_sst_save, aud_sst_load))

#endif /* _CORE_AUD_MIX_H_ */
