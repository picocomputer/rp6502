/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_AUD_OPL_H_
#define _CORE_AUD_OPL_H_

/* OPL2 sound generator
 */

#include "core/sys/sst.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* One mono sample at AUD_NATIVE_RATE, which is a YM3812's own rate. */
int16_t opl_sample(void);

/* The one voice this chip has, on both sides: what a mixer takes when the
 * OPL is the device. */
void opl_stereo(int16_t *left, int16_t *right);

bool opl_xreg(uint16_t word);

/* Where this engine's register page sits in XRAM, 0xFFFF for parked. */
uint16_t opl_xaddr_get(void);

/* Release the pointer without touching the mixer, for the other engine
 * taking over. The fabric parks the same way: a write to either engine's
 * pointer register releases the other. */
void opl_park(void);

/* The YM3812 as it stands. Every member of the emulation but the eight this
 * build fixes -- the clock, the rate, the three step fields the two of them
 * divide out to, the pan and mask a program here never moves, the mix output
 * a sample overwrites, and the four timer callbacks, which are addresses.
 * Each slot's patch points at its own slot and its wave row is one of four,
 * so both ride as what they are rather than as pointers.
 *
 * 1 present, 2 pointer, 256 registers, 9 algorithms, 38 chip scalars,
 * 15 channel outputs of 2, then 18 slots.
 *
 * A slot is 1 number, 1 type, 13 patch, 8 output, 1 wave row, 4 phase,
 * 4 phase out, 1 keep, 2 blk_fnum, 2 fnum, 1 blk, 1 envelope state, 2 tll,
 * 1 rks, 2 rates, 4 shift, 2 out, 4 update requests. */
#define OPL_SLOT_SST_SIZE 54
#define OPL_SST_SIZE (1 + 2 + 256 + 9 + 38 + 15 * 2 + 18 * OPL_SLOT_SST_SIZE)
void opl_sst_save(sst_cursor_t *c, unsigned flags);
bool opl_sst_load(sst_cursor_t *c, unsigned flags);

/* This driver's row in a machine's driver list; see core/sys/driver.h. It
 * carries nothing but the chunk, as the PSG's does. */
#define OPL_DRIVER DRIVER(nul_init, nul_task, nul_task, nul_run, nul_stop, nul_break, \
    nul_config, nul_config, SST(OPL_, 1, OPL_SST_SIZE, opl_sst_save, opl_sst_load))

#endif /* _CORE_AUD_OPL_H_ */
