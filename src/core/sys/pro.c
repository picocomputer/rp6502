/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "core/sys/pro.h"
#include "core/api/pro.h"
#include "core/sys/msc.h"
#include "host/fs.h"
#include "core/sys/mem.h"
#include "core/sys/cpu.h"
#include "core/api/api.h"
#include "core/api/arg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pending exec (op 0x09): the new program loads at the frame boundary rather
 * than mid-tick, so the master clock and the partially-run frame stay
 * consistent. pro_exec() captures the ROM path and stops the current program;
 * the frame loop commits it via pro_take_exec(). */
static bool exec_pending;
static char exec_path[MSC_MAX_PATH];

void pro_init(void)
{
    exec_pending = false;
}

void pro_exec(const char *rom_path)
{
    snprintf(exec_path, sizeof(exec_path), "%s", rom_path);
    exec_pending = true;
    cpu_set_halted(true); /* stop the current program; the tick loop exits */
}

const char *pro_take_exec(void)
{
    if (!exec_pending)
        return NULL;
    exec_pending = false;
    return exec_path;
}

bool pro_exec_pending(void)
{
    return exec_pending;
}

/* Seed the initially loaded program's argv (firmware rom_load_argv/rom_exec).
 * argv[0] is the program's own path in 6502 form so it can re-exec itself: a
 * drive path or an installed ":name" is used verbatim; a host path maps back
 * through realpath. Empty args are kept, like the monitor's LOAD. */
bool pro_set_argv(const char *rom, int argc, char *const *args)
{
    char abs[MSC_MAX_PATH], msc[MSC_MAX_PATH];
    const char *argv0 = rom;
    if (!msc_has_drive_prefix(rom) && rom[0] != ':' && fs_realpath(rom, abs, sizeof(abs)))
    {
        msc_from_host(abs, msc, sizeof(msc));
        argv0 = msc;
    }
    /* Length-guard each string: arg_append's uint16 math trusts
     * monitor-capped tokens, but host input is unbounded. */
    arg_clear();
    bool ok = strlen(argv0) < XSTACK_SIZE && arg_append(argv0);
    for (int i = 0; ok && i < argc; i++)
        ok = strlen(args[i]) < XSTACK_SIZE && arg_append(args[i]);
    if (!ok)
        arg_clear(); /* no partial argv; the caller decides severity */
    pro_run(); /* the initial program is now what's running */
    return ok;
}

/* Program EXIT (op 0xFF). Records the code, then the shared chain decides
 * whether there is a launcher to go back to; false means nothing is left to
 * run and the caller halts. */
bool pro_exit(int16_t exit_code)
{
    pro_set_exit_code(exit_code);
    return pro_stop();
}

/* Both are the same note here: this machine loads at the frame boundary
 * rather than mid-tick, where the master clock and a half-run frame would
 * disagree. pro_exec halts the 6502, which is all the stopping op 0x09
 * needs. */
void pro_exec_start(const char *path)
{
    pro_exec(path);
}

void pro_exec_relaunch(const char *path)
{
    pro_exec(path);
}

/* Never: a pending exec on this machine is the note the frame loop is about
 * to read, including the one the chain itself just wrote. A load someone else
 * committed cannot be outstanding at a stop, because an exec halts the 6502
 * on the spot rather than letting it reach EXIT. */
bool pro_exec_inflight(void)
{
    return false;
}
