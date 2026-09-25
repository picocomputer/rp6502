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
#include <string.h>

static char proc_argv0[API_PATH_MAX + 1];
static bool proc_exec_pending;

void proc_restage(void)
{
    proc_exec_pending = false;
    proc_cancel_launcher();
    arg_clear();
    proc_run();
    memcpy(proc_argv0, FS_DRIVE, sizeof FS_DRIVE - 1);
    if (fs_getfile(FS_SLOT_ROM, proc_argv0 + sizeof FS_DRIVE - 1,
                   sizeof proc_argv0 - (sizeof FS_DRIVE - 1)))
        arg_append(proc_argv0);
    else
        proc_argv0[0] = 0;
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

/* An exec leaves argv[0] as the program wrote it, so it is made absolute
 * against the working directory here, to record where the ROM was. A ':name'
 * is left as written, because it is not a path. The function is kept out of
 * line so that abs takes stack only during an exec, rather than in main's
 * frame for as long as the firmware runs. */
__attribute__((noinline)) static bool proc_argv0_absolute(void)
{
    const char *path = arg_index(0);
    if (path[0] == ':')
        return true;
    char abs[API_PATH_MAX + 1];
    memcpy(abs, FS_DRIVE, sizeof FS_DRIVE - 1);
    return fs_card_path(fs_strip_drive(path), FS_ASSETS_PATH,
                        abs + sizeof FS_DRIVE - 1,
                        sizeof abs - (sizeof FS_DRIVE - 1))
           && arg_replace(0, abs);
}

bool proc_exec_take(void)
{
    if (!proc_exec_pending)
        return false;
    proc_exec_pending = false;
    if (!proc_argv0_absolute())
    {
        printf("argv does not fit for ROM '%s'\n", arg_index(0));
        return false;
    }
    const char *path = arg_index(0);
    if (!rom_load(path))
    {
        printf("exec: no %s\n", path);
        return false;
    }
    return true;
}
