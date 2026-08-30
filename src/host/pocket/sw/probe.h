/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_PROBE_H_
#define _FPGA_SW_PROBE_H_

/* What the host actually does, asked on the device.
 *
 * Analogue documents the commands and none of the lifecycle: whether a
 * runtime binding survives a wake, whether a savestate load touches slot
 * 0, what state the bridge is in when the core comes back. Every one of
 * those is load-bearing in fs.c and sst.c and every one of them is an
 * assumption. This driver reads the same facts at every phase of a
 * session and prints them the same way each time, so the answer is the
 * difference between two columns rather than one reading and a story.
 *
 * Built only for the probe package, so a shipping build carries none of
 * it -- the readings cost bridge round trips, which is the one thing the
 * drive and the staging store both need.
 */

#include "core/driver.h"

#ifdef RP6502_POCKET_PROBE

/* The boot readings, taken on the first instructions and printed at
 * once. A wake is a core launch, so this runs before the blob lands and
 * these are the only readings of it that exist: the engine halts this
 * CPU to write the sleeping session's TCM over ours, and everything held
 * in memory becomes that session's. The console copy has already left
 * through the fabric by then. */
void probe_init(void);

/* Watches for the events that end a phase -- a restore completing, the
 * host re-announcing a slot -- and runs the suite on each. */
void probe_task(void);

/* One Get File on the ROM slot, tagged with the moment it was taken.
 *
 * The fabric's "the host wrote into my window" bit fires for the ask
 * main_stage makes and for none of the asks that come later, though the
 * host answers every one of them correctly. That looks less like a race
 * than like a bit that reports the host pushing rather than the host
 * answering -- true only while the host is the one driving. This is how
 * that gets settled: the same ask at known moments, from the first
 * instruction through the host-driven staging and out into the idle
 * seconds after it, with the bit printed each time.
 *
 * main.c calls it with "stage", which also starts the ladder; a ROM
 * swap re-enters main_stage and so asks again while the host drives. */
void probe_mark(const char *when);

#else

static inline void probe_init(void) {}
static inline void probe_task(void) {}
static inline void probe_mark(const char *when) { (void)when; }

#endif

/* This driver's row in a machine's driver list; see core/driver.h.
 * io_task, not task: the readings are bridge round trips. */
#define PROBE_DRIVER DRIVER(probe_init, nul_task, probe_task, nul_run, nul_stop, nul_break)

#endif /* _FPGA_SW_PROBE_H_ */
