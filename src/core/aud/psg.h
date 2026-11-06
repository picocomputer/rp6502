/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_AUD_PSG_H_
#define _CORE_AUD_PSG_H_

#include "core/sys/sst.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* The channel block: eight channels of eight bytes. */
#define PSG_REGS_SIZE 64

/* One stereo sample from the engine's channel structures, the bell's
 * included, and the engine advanced by one. The phase and envelope steps
 * are derived from AUD_NATIVE_RATE, so a mixer has to call this once per
 * sample at that rate, or skip it while the PSG is parked and
 * psg_bel_idle. */
void psg_sample(int16_t *left, int16_t *right);

/* Byte i of the program's channel block, as XRAM holds it, with no gate
 * struck: what a map copies into the engine. */
void psg_reg_put(uint8_t i, uint8_t val);

/* Byte i of the bell's channel structure, from the bell's sequencer. Byte 6
 * strikes or releases its gate. */
void psg_bel_put(unsigned i, uint8_t val);

/* True while the bell steps to the same state every sample. */
bool psg_bel_idle(void);

/* One write the RW engine landed on this engine's page, the low byte of its
 * address and its value. A write to the block lands in the engine's copy,
 * and one to a gate strikes or releases it. */
void psg_xram_write(uint8_t loc, uint8_t val);

/* Checks the word and hands it to aud_psg_map, 0xFFFF for any that is not a
 * channel block's start. */
bool psg_xreg(uint16_t word);

/* Resets the eight channels and points the engine at the block at word,
 * 0xFFFF for parked. Reached through aud_psg_map, which then copies the
 * block in with psg_reg_put. */
void psg_map(uint16_t word);

/* Where this engine's channel block sits in XRAM, 0xFFFF for parked. */
uint16_t psg_xaddr_get(void);

/* Release the pointer, which takes the eight channels out of the mix and
 * leaves the bell. The caller holds the engine lock. */
void psg_park(void);

/* The pointer, the eight channels behind it and the bell. The channel
 * blocks themselves are in XRAM, so the XRAM row carries them and this one
 * does not; the bell's structure is here.
 *
 * 2 pointer, then 9 voices of 2 sample, 1 adsr, 4 vol, 4 phase, 2+2
 * noise, 2 freq, 4 increment, then the bell's 7 bytes. */
#define PSG_SST_SIZE (2 + 9 * 21 + 7)
void psg_sst_save(sst_cursor_t *c, unsigned flags);
bool psg_sst_load(sst_cursor_t *c, unsigned flags);

#define PSG_DRIVER DRIVER(nul_init, nul_task, nul_task, nul_run, nul_stop, nul_break, \
    nul_config, nul_config, SST(PSG_, 2, PSG_SST_SIZE, psg_sst_save, psg_sst_load))

#endif /* _CORE_AUD_PSG_H_ */
