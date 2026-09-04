/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/sys.h"
#include "core/sys/proc.h"
#include "core/rom/rom.h"
#include "core/api/proc.h"
#include "core/api/arg.h"
#include "core/ria/regs.h"
#include "core/wdc/sram.h"
#include "core/sys/xram.h"
#include "core/wdc/resb.h"
#include "core/str/path.h"
#include "osal/dir.h"
#include "osal/os.h"
#include <stdlib.h>
#include <string.h>

/* An exec argv[0] already names, waiting for a frame boundary. */
static bool queued;

void proc_exec_init(void)
{
    queued = false;
}

void proc_exec_request(void)
{
    queued = true;
    resb_assert(); /* stop the current program; the tick loop exits */
}

bool proc_set_argv(const char *rom, int argc, char *const *args)
{
    /* realpath answers in the 6502's spelling, so an absolute host path comes
     * back as the drive path a program can hand straight back to exec. */
    char *abs = (!path_has_drive(rom) && rom[0] != ':') ? os_dir_realpath(rom) : NULL;
    const char *argv0 = abs ? abs : rom;
    /* Length-guard each string: arg_append's uint16 math trusts
     * monitor-capped tokens, but host input is unbounded. */
    arg_clear();
    bool ok = strlen(argv0) < XSTACK_SIZE && arg_append(argv0);
    for (int i = 0; ok && i < argc; i++)
        ok = strlen(args[i]) < XSTACK_SIZE && arg_append(args[i]);
    if (!ok)
        arg_clear(); /* no partial argv; the caller decides severity */
    free(abs);
    proc_run(); /* the new program is now what's running */
    return ok;
}

bool proc_boot(const char *rom, int argc, char *const *args, unsigned flags)
{
    sys_stop_now(); /* before the load writes what the outgoing program ran on */
    /* Whatever the outgoing program queued on its way out goes with it. The
     * stop above ran proc_stop, which arms a launcher relaunch when there is
     * a chain -- and a start that was asked for by name is not that child. */
    queued = false;
    if (flags & PROC_REFILL)
    {
        sram_init();
        xram_init();
    }
    if (!rom_load(rom)) /* rom_load says why; a caller adding to it says it twice */
        return false;
    if (argc >= 0)
        proc_set_argv(rom, argc, args);
    if (flags & PROC_UNCHAIN)
        proc_set_launcher(false);
    sys_run();
    return true;
}

/* Both are the same note here: this machine loads at the frame boundary
 * rather than mid-tick, where the clock and a half-run frame would disagree.
 * proc_exec_request halts the 6502, which is all the stopping op 0x09 needs. */
void proc_exec_start(void)
{
    proc_exec_request();
}

void proc_exec_relaunch(void)
{
    proc_exec_request();
}

/* A queued exec is a load this machine has committed to, including the one
 * the chain just wrote: the launcher must not be put over it. proc_boot
 * clears the queue after the stop walk has read this, so a start by name
 * still wins. */
bool proc_exec_inflight(void)
{
    return queued;
}

void proc_exec_task(void)
{
    if (!queued)
        return;
    /* argv[0], where the request left it: the stop walk inside proc_boot
     * leaves argv alone while an exec is in flight. */
    if (!proc_boot(arg_index(0), -1, NULL, 0))
        proc_set_exit_code(1); /* stays stopped from proc_boot's stop */
}
