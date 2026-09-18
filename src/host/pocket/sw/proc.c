/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "fs.h"
#include "proc.h"
#include "rom.h"

#include "core/api/api.h"
#include "core/api/arg.h"
#include "core/api/proc.h"
#include "core/sys/sys.h"

#include <stdio.h>

#define PROC_ARGV0_MAX 128

static char proc_argv0[PROC_ARGV0_MAX];
static bool proc_exec_pending;

void proc_restage(void)
{
    proc_exec_pending = false;
    proc_cancel_launcher();
    arg_clear();
    proc_run();
    if (fs_getfile(FS_SLOT_ROM, proc_argv0, sizeof proc_argv0))
        arg_append(proc_argv0);
}

const char *proc_staged_path(void)
{
    return proc_running()[0] ? proc_running() : proc_argv0;
}

void proc_exec_start(void)
{
    proc_exec_relaunch();
    sys_stop();
}

void proc_exec_relaunch(void)
{
    proc_exec_pending = true;
}

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
