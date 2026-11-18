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

void proc_exec_drop(void)
{
    /* The stop an exec made did not end the chain, so it ends here. */
    if (proc_exec_pending)
    {
        proc_exec_pending = false;
        proc_chain_end();
    }
    proc_cancel_launcher();
}

void proc_restage(void)
{
    /* A start by name overrides an exec still on its way. */
    proc_exec_drop();
    arg_clear();
    proc_run();
    memcpy(proc_argv0, FS_DRIVE, sizeof FS_DRIVE - 1);
    if (fs_getfile(FS_SLOT_ROM, proc_argv0 + sizeof FS_DRIVE - 1,
                   sizeof proc_argv0 - (sizeof FS_DRIVE - 1)))
        arg_append(proc_argv0);
    else
        proc_argv0[0] = 0;
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
 * here, to record where the ROM was. A relative path is taken under
 * FS_ASSETS_PATH, where the ROMs are, and not under the working directory:
 * an exec is the one call that does. A ':name' is left as written, because
 * it is not a path. The function is kept out of line so that abs takes
 * stack only during an exec, rather than in main's frame for as long as
 * the firmware runs. */
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
        proc_exec_failed();
        printf("argv does not fit for ROM '%s'\n", arg_index(0));
        return false;
    }
    const char *path = arg_index(0);
    if (!rom_load(path))
    {
        proc_exec_failed();
        printf("exec: no %s\n", path);
        return false;
    }
    return true;
}

void proc_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    sst_put_str(c, proc_argv0, sizeof proc_argv0);
    sst_put_bool(c, proc_exec_pending);
    proc_chain_sst_save(c);
}

bool proc_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    const char *argv0 = sst_view_str(c, sizeof proc_argv0);
    bool pending = sst_get_bool(c);
    if (!sst_ok(c))
        return false;
    if (!proc_chain_sst_load(c))
        return sst_refuse(c, "the program's name and arguments are out of range");
    strcpy(proc_argv0, argv0);
    proc_exec_pending = pending;
    return true;
}
