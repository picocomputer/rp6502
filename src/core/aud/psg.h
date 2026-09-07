/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_AUD_PSG_H_
#define _CORE_AUD_PSG_H_

/* Programmable Sound Generator
 */

#include "core/sys/sst.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* One stereo sample at AUD_NATIVE_RATE, and the engine advanced by one. */
void psg_sample(int16_t *left, int16_t *right);

bool psg_xreg(uint16_t word);

/* Where this engine's channel block sits in XRAM, 0xFFFF for parked. */
uint16_t psg_xaddr_get(void);

/* Release the pointer without touching the mixer, for the other engine
 * taking over. See opl_park. */
void psg_park(void);

/* The pointer and the eight channels behind it. Carried exactly: the phase,
 * the envelope and both noise words are what a note in flight sounds like,
 * and an engine that started over on load would click on every rewound
 * frame. The block itself is in XRAM, which its own row carries.
 *
 * 2 pointer, then 8 channels of 2 sample, 1 adsr, 4 vol, 4 phase, 4+4
 * noise, 2 freq, 4 increment. */
#define PSG_SST_SIZE (2 + 8 * 25)
void psg_sst_save(sst_cursor_t *c, unsigned flags);
bool psg_sst_load(sst_cursor_t *c, unsigned flags);

/* This driver's row in a machine's driver list; see core/sys/driver.h. It
 * carries nothing but the chunk: the engine starts and stops through its
 * xreg, and the mixer's row is what holds the device. */
#define PSG_DRIVER DRIVER(nul_init, nul_task, nul_task, nul_run, nul_stop, nul_break, \
    nul_config, nul_config, SST(PSG_, 1, PSG_SST_SIZE, psg_sst_save, psg_sst_load))

#endif /* _CORE_AUD_PSG_H_ */
