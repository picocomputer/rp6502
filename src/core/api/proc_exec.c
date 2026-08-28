/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "core/mach.h"
#include "core/sys/log.h"
#include "core/rom/rom.h"
#include "core/api/proc_exec.h"
#include "core/api/proc.h"
#include "core/api/fs.h"
#include "core/str/path.h"
#include "host/os.h"
#include "core/mem/mem.h"
#include "core/wdc/cpu.h"
#include "core/api/api.h"
#include "core/api/arg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pending exec (op 0x09): the new program loads at the frame boundary rather
 * than mid-tick, so the master clock and the partially-run frame stay
 * consistent. proc_exec() captures the ROM path and stops the current program;
 * proc_exec_task() commits it. */
static bool exec_pending;
static char exec_path[HOST_MAX_PATH];

void proc_init(void)
{
    exec_pending = false;
}

void proc_exec(const char *rom_path)
{
    snprintf(exec_path, sizeof(exec_path), "%s", rom_path);
    exec_pending = true;
    cpu_set_halted(true); /* stop the current program; the tick loop exits */
}

static const char *proc_take_exec(void)
{
    if (!exec_pending)
        return NULL;
    exec_pending = false;
    return exec_path;
}

bool proc_exec_pending(void)
{
    return exec_pending;
}

/* Seed the initially loaded program's argv (firmware rom_load_argv/rom_exec).
 * argv[0] is the program's own path in 6502 form so it can re-exec itself: a
 * drive path or an installed ":name" is used verbatim; a host path maps back
 * through realpath. Empty args are kept, like the monitor's LOAD. */
bool proc_set_argv(const char *rom, int argc, char *const *args)
{
    char abs[HOST_MAX_PATH];
    const char *argv0 = rom;
    /* realpath answers in the 6502's spelling, so an absolute host path comes
     * back as the drive path a program can hand straight back to exec. */
    if (!path_has_drive(rom) && rom[0] != ':' && host_fs_realpath(rom, abs, sizeof(abs)))
        argv0 = abs;
    /* Length-guard each string: arg_append's uint16 math trusts
     * monitor-capped tokens, but host input is unbounded. */
    arg_clear();
    bool ok = strlen(argv0) < XSTACK_SIZE && arg_append(argv0);
    for (int i = 0; ok && i < argc; i++)
        ok = strlen(args[i]) < XSTACK_SIZE && arg_append(args[i]);
    if (!ok)
        arg_clear(); /* no partial argv; the caller decides severity */
    proc_run(); /* the initial program is now what's running */
    return ok;
}

/* Program EXIT (op 0xFF). Records the code, then the shared chain decides
 * whether there is a launcher to go back to; false means nothing is left to
 * run and the caller halts. */
bool proc_exit(int16_t exit_code)
{
    proc_set_exit_code(exit_code);
    return proc_stop();
}

/* Both are the same note here: this machine loads at the frame boundary
 * rather than mid-tick, where the master clock and a half-run frame would
 * disagree. proc_exec halts the 6502, which is all the stopping op 0x09
 * needs. */
void proc_exec_start(const char *path)
{
    proc_exec(path);
}

void proc_exec_relaunch(const char *path)
{
    proc_exec(path);
}

/* Never: a pending exec on this machine is the note proc_exec_task is about
 * to read, including the one the chain itself just wrote. A load someone else
 * committed cannot be outstanding at a stop, because an exec halts the 6502
 * on the spot rather than letting it reach EXIT. */
bool proc_exec_inflight(void)
{
    return false;
}

/* Commit a queued exec: put the outgoing program away, load the incoming one
 * over the RAM it was running out of, and ask for the machine back. Both asks
 * go through the latch, so the walk that is running right now finishes first
 * -- which is the whole reason the latch exists. */
void proc_exec_task(void)
{
    const char *path = proc_take_exec();
    if (!path)
        return;
    mach_stop();
    mach_commit(); /* the load below writes what the outgoing program ran on */
    if (!rom_load(path))
    {
        log_error("exec failed to load '%s'", path);
        proc_set_exit_code(1); /* stays halted from the stop above */
        return;
    }
    mach_run(); /* the pass's own commit starts it */
}
