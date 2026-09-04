/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * How this machine starts a program, which is the half of core/api/proc.h
 * every machine answers differently: the RIA streams a ROM to the 6502 over
 * its bus across many passes, the Pocket stages an image through the APF
 * bridge, and this one loads straight into sram[] because it owns the RAM.
 *
 * Everything that starts a program here comes through proc_boot -- a dropped
 * file, a frontend's load, the command line, a test, and an exec the running
 * program asked for. That is the point of it: what has to be true of a start
 * is then true of all of them.
 */

#ifndef _CORE_SYS_PROC_H_
#define _CORE_SYS_PROC_H_

#include "core/api/proc.h"
#include <stdbool.h>
#include <stdint.h>

/* Stand a program up: put the outgoing one away, load the image over the RAM
 * it was running out of, seed its argv, and ask for the machine back.
 *
 * argc < 0 leaves argv alone, which is what an exec wants -- the outgoing
 * program wrote it on its way out. Returns false with the machine left
 * stopped: rom_load streams into live RAM, so a failure may already have
 * written over what was running.
 *
 * Ends at the ask, not the doing: a caller inside a driver walk lets the
 * pass commit, and a host outside one calls sys_commit itself. */
#define PROC_REFILL 0x01  /* the fills first: a fresh machine, not a program change */
#define PROC_UNCHAIN 0x02 /* break any launcher chain; a chosen program is not a child */
bool proc_boot(const char *rom, int argc, char *const *args, unsigned flags);

/* Seed a program's argv without starting it: its own path + args, in the
 * 6502's spelling so it can hand argv[0] back to exec. False on overflow. */
bool proc_set_argv(const char *rom, int argc, char *const *args);

/* Ask for an exec of what argv[0] names at the next frame boundary rather
 * than mid-tick, where the clock and a half-run frame would disagree. Stops
 * the current program; proc_exec_task performs it, and proc_exec_inflight
 * answers true until it has. */
void proc_exec_request(void);

void proc_exec_init(void); /* nothing an earlier run asked for survives a cold boot */
void proc_exec_task(void); /* perform a requested exec */

/* This machine's proc row; see core/sys/driver.h. The chain's columns over
 * core/api/proc.c, and the exec's: performed in the io column because loading
 * a ROM reads a file. */
#define PROC_DRIVER DRIVER(proc_exec_init, nul_task, proc_exec_task, proc_run, proc_stop, nul_break, nul_config, nul_config)

#endif /* _CORE_SYS_PROC_H_ */
