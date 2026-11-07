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

/* A block may start anywhere it ends inside XRAM. */
static inline bool psg_block_ok(uint16_t word)
{
    return word <= 0x10000 - PSG_REGS_SIZE;
}

/* One stereo sample from the engine's channel structures, the bell's
 * included, and the engine advanced by one. The phase and envelope steps
 * are derived from AUD_NATIVE_RATE, so a mixer has to call this once per
 * sample at that rate, or skip it while the PSG is parked and
 * psg_bel_idle. */
void psg_sample(int16_t *left, int16_t *right);

/* True while the bell steps to the same state every sample. */
bool psg_bel_idle(void);

/* Byte offset of the nine voices' structures: the program's eight at 0 to
 * 63, where the machine found the write in the block, then the bell's from
 * PSG_REGS_SIZE, from its sequencer. A write to a gate byte strikes or
 * releases that voice. */
void psg_put(unsigned offset, uint8_t val);

/* Resets the eight channels, their registers included, and points the
 * engine at the block at word, 0xFFFF for parked. Reached through
 * aud_psg_map. */
void psg_map(uint16_t word);

/* Where this engine's channel block sits in XRAM, 0xFFFF for parked. */
uint16_t psg_xaddr_get(void);

/* Release the pointer, which takes the eight channels out of the mix and
 * leaves the bell. The mixer calls it with its engines held still. */
void psg_park(void);

/* The pointer, the eight channels behind it and the bell. XRAM need not
 * hold what the engine does, so the registers are here too.
 *
 * 2 pointer, then 9 voices of 2 sample, 1 adsr, 4 vol, 4 phase, 2+2
 * noise, then the 9 voices' registers, 8 bytes each. */
#define PSG_SST_SIZE (2 + 9 * 15 + 9 * 8)
void psg_sst_save(sst_cursor_t *c, unsigned flags);
bool psg_sst_load(sst_cursor_t *c, unsigned flags);

#define PSG_DRIVER DRIVER(nul_init, nul_task, nul_task, nul_run, nul_stop, nul_break, \
    nul_config, nul_config, SST(PSG_, 3, PSG_SST_SIZE, psg_sst_save, psg_sst_load))

#endif /* _CORE_AUD_PSG_H_ */
