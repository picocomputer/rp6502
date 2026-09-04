/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * argv and exec. The core is never told what it was called, so argv[0]
 * is asked for with Get File on the ROM slot and kept exactly as the
 * host spells it, absolute path and all.
 *
 * Exec stages the next image where the host would have put it, so its
 * bundled assets arrive with it and rom.c needs to know nothing about
 * how it got there. The load happens after the machine has stopped
 * rather than inside the syscall, because stopping closes the
 * descriptors the outgoing program left open and the read needs one.
 */

#include "fs.h"
#include "proc.h"
#include "rom.h"

#include "core/api/api.h"
#include "core/api/arg.h"
#include "core/api/proc.h"
#include "core/sys/sys.h"

#include <stdio.h>

/* Short of the host's 256-byte field because this is static RAM. */
#define PROC_ARGV0_MAX 128

static char proc_argv0[PROC_ARGV0_MAX];
static bool proc_exec_pending;

/* Once per staged image, not once per run: asking is a blocking bridge
 * command and a run does not change the answer. Never on an exec, where
 * the argument buffer already holds what the outgoing program passed. */
void proc_restage(void)
{
    proc_exec_pending = false;
    /* A program the user picked from the menu supersedes the chain the
     * one before it was in. */
    proc_cancel_launcher();
    arg_clear();
    proc_run(); /* an empty argv: nothing is running until the image starts */
    if (fs_getfile(FS_SLOT_ROM, proc_argv0, sizeof proc_argv0))
        arg_append(proc_argv0);
}

/* An exec staged its own image, so the chain's path is the one the
 * store holds; before any exec that is the host's answer. */
const char *proc_staged_path(void)
{
    return proc_running()[0] ? proc_running() : proc_argv0;
}

/* An exec the program asked for wins; otherwise it returns to the
 * launcher. The launcher's own exit ends the chain. Both leave the image to
 * proc_exec_take, which reads argv[0]; op 0x09 also stops the machine,
 * because the program that asked is already gone and the staging store is
 * the console's competitor for the bridge. The relaunch is inside a stop
 * already. */
void proc_exec_start(void)
{
    proc_exec_relaunch();
    sys_stop();
}

void proc_exec_relaunch(void)
{
    proc_exec_pending = true;
}

/* A staged image is a load this machine has committed to: the chain must
 * not put the launcher over it. */
bool proc_exec_inflight(void)
{
    return proc_exec_pending;
}

bool proc_exec_take(void)
{
    if (!proc_exec_pending)
        return false;
    proc_exec_pending = false;
    const char *path = arg_index(0);
    if (!rom_load(path))
    {
        printf("exec: no %s\n", path);
        return false;
    }
    return true;
}
