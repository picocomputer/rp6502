/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/mach.h"
#include "core/sys/exec.h"
#include "core/sys/log.h"
#include "core/rom/rom.h"
#include "core/api/proc.h"
#include "core/api/arg.h"
#include "core/mem/mem.h"
#include "core/wdc/cpu.h"
#include "core/str/path.h"
#include "host/os.h"
#include <stdio.h>
#include <string.h>

/* The exec a program asked for, waiting for a frame boundary. */
static bool queued;
static char queued_path[HOST_MAX_PATH];

void exec_init(void)
{
    queued = false;
}

void exec_request(const char *rom_path)
{
    snprintf(queued_path, sizeof(queued_path), "%s", rom_path);
    queued = true;
    cpu_set_halted(true); /* stop the current program; the tick loop exits */
}

bool exec_pending(void)
{
    return queued;
}

bool exec_set_argv(const char *rom, int argc, char *const *args)
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
    proc_run();       /* the new program is now what's running */
    return ok;
}

bool exec_boot(const char *rom, int argc, char *const *args, unsigned flags)
{
    mach_stop();
    mach_commit(); /* before the load writes what the outgoing program ran on */
    /* Whatever the outgoing program queued on its way out goes with it. The
     * stop above ran proc_stop, which arms a launcher relaunch when there is
     * a chain -- and a start that was asked for by name is not that child. */
    queued = false;
    if (flags & EXEC_REFILL)
        mem_init();
    if (!rom_load(rom)) /* rom_load says why; a caller adding to it says it twice */
        return false;
    if (argc >= 0)
        exec_set_argv(rom, argc, args);
    if (flags & EXEC_UNCHAIN)
        proc_set_launcher(false);
    mach_run();
    return true;
}

/* Program EXIT (op 0xFF). The code, then the stop -- the chain is the stop
 * walk's to decide, exactly as it is on the firmwares. */
void proc_exit(int16_t exit_code)
{
    proc_set_exit_code(exit_code);
    mach_stop();
}

/* Both are the same note here: this machine loads at the frame boundary
 * rather than mid-tick, where the clock and a half-run frame would disagree.
 * exec_request halts the 6502, which is all the stopping op 0x09 needs. */
void proc_exec_start(const char *path)
{
    exec_request(path);
}

void proc_exec_relaunch(const char *path)
{
    exec_request(path);
}

/* A queued exec is a load this machine has committed to, including the one
 * the chain just wrote: the launcher must not be put over it. exec_boot
 * clears the queue after the stop walk has read this, so a start by name
 * still wins. */
bool proc_exec_inflight(void)
{
    return queued;
}

void exec_task(void)
{
    if (!queued)
        return;
    /* queued_path outlives the clear inside exec_boot -- only the flag is
     * dropped, and nothing writes the buffer until the next request. */
    if (!exec_boot(queued_path, -1, NULL, 0))
        proc_set_exit_code(1); /* stays stopped from exec_boot's stop */
}
