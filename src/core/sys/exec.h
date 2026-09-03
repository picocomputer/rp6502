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
 * Everything that starts a program here comes through exec_boot -- a dropped
 * file, a frontend's load, the command line, a test, and an exec the running
 * program asked for. That is the point of it: what has to be true of a start
 * is then true of all of them.
 */

#ifndef _CORE_SYS_EXEC_H_
#define _CORE_SYS_EXEC_H_

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
#define EXEC_REFILL 0x01  /* the fills first: a fresh machine, not a program change */
#define EXEC_UNCHAIN 0x02 /* break any launcher chain; a chosen program is not a child */
bool exec_boot(const char *rom, int argc, char *const *args, unsigned flags);

/* Seed a program's argv without starting it: its own path + args, in the
 * 6502's spelling so it can hand argv[0] back to exec. False on overflow. */
bool exec_set_argv(const char *rom, int argc, char *const *args);

/* Ask for an exec at the next frame boundary rather than mid-tick, where the
 * clock and a half-run frame would disagree. Stops the current program;
 * exec_task performs it. */
void exec_request(const char *rom_path);
bool exec_pending(void); /* queued, not yet performed */

/* Program EXIT (op 0xFF): record the code and stop. What happens next is the
 * chain's, decided by the stop walk -- a launcher to go back to, or nothing
 * left to run. */
void proc_exit(int16_t exit_code);

void exec_init(void); /* clear any pending exec (cold boot) */
void exec_task(void); /* perform a queued exec */

/* This driver's row in a machine's driver list; see core/sys/driver.h. The queue's
 * columns, beside core/api/proc.h's row for the chain: an exec is performed
 * in the io column because loading a ROM reads a file. */
#define EXEC_DRIVER DRIVER(exec_init, nul_task, exec_task, nul_run, nul_stop, nul_break, nul_config, nul_config)

#endif /* _CORE_SYS_EXEC_H_ */
