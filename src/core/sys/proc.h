/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * How this machine starts a program, which is the half of core/api/proc.h that
 * every machine answers differently. This one loads straight into sram[],
 * because it owns the RAM the 6502 runs out of.
 */

#ifndef _CORE_SYS_PROC_H_
#define _CORE_SYS_PROC_H_

#include "core/api/proc.h"
#include "core/sys/sst.h"
#include <stdbool.h>
#include <stdint.h>

/* Stand a program up: put the outgoing one away, load the image over the RAM
 * it was running out of, seed its argv, and ask for the machine back.
 *
 * argc < 0 leaves argv alone, which is what an exec wants, the outgoing
 * program having written it on its way out. Returns false with the machine
 * left stopped: rom_load deposits records into live RAM as it reads them, so a
 * failure may already have written over what was running.
 *
 * It ends at the request rather than the start, so a caller inside a driver
 * walk leaves the pass to commit it and a host outside one calls sys_commit
 * itself. */
#define PROC_REFILL 0x01  /* fill sram and xram first: a fresh machine, not a program change */
#define PROC_UNCHAIN 0x02 /* break any launcher chain, because a program asked for by name is not a child */
bool proc_boot(const char *rom, int argc, char *const *args, unsigned flags);

/* Seed a program's argv without starting it: its own path, made absolute
 * unless it is a ':name' installed ROM, then the args. False when they do not
 * fit. */
bool proc_set_argv(const char *rom, int argc, char *const *args);

/* Ask for an exec of what argv[0] names. The 6502 stops here, but the load
 * waits for proc_exec_task in the io column, so a program's RAM is never
 * written over from inside the syscall that asked for it. proc_exec_inflight
 * answers true until the load has happened. */
void proc_exec_request(void);

void proc_exec_init(void);
void proc_exec_task(void);

/* The program is gone and nothing is on its way: RESB low with no exec or
 * launcher relaunch queued. RESB alone is not enough, because it is also low
 * between the request for an exec and the load that performs it. */
bool proc_exited(void);

/* This machine's proc row; see core/sys/driver.h. The exec is performed in the
 * io column because loading a ROM reads a file.
 *
 * The slot holds the queued flag, the exit code, the two paths of the chain and
 * the whole argv buffer: 1 + 2 + 256 + 256 + XSTACK_SIZE. */
#define PROC_SST_SIZE (515 + XSTACK_SIZE)
#define PROC_PATH_SLOT 256
void proc_sst_save(sst_cursor_t *c, unsigned flags);
bool proc_sst_load(sst_cursor_t *c, unsigned flags);

#define PROC_DRIVER DRIVER(proc_exec_init, nul_task, proc_exec_task, proc_run, proc_stop, nul_break, \
    nul_config, nul_config, SST(PROC, 1, PROC_SST_SIZE, proc_sst_save, proc_sst_load))

#endif /* _CORE_SYS_PROC_H_ */
