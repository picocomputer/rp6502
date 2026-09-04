/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The launcher chain: which program is running, which one to return to
 * when it exits, and what it exited with. Every machine ran the same rules
 * -- a program registers itself, its children come back to it, and the
 * chain ends when the launcher itself stops -- and each had written them
 * out. What actually differs is how a machine starts the next program --
 * proc_exec_start for one the running program asked for, proc_exec_relaunch
 * for the launcher coming back -- and whether one is already on its way,
 * which is proc_exec_inflight. argv[0] names the program in every case, so
 * neither carries a path.
 */

#include "core/api/proc.h"
#include "core/api/api.h"
#include "core/api/arg.h"
#include "core/sys/sys.h"
#include "osal/dir.h"

#include <string.h>

/* argv[0] of the program running now, and of the one to come back to: copies
 * the drive keeps, NULL while there is none. */
static char *proc_running_path;
static char *proc_launcher_path;
static int16_t proc_exit_code;

static void set_path(char **slot, const char *path)
{
    if (*slot)
        os_dir_path_drop(*slot);
    *slot = path && path[0] ? os_dir_path_hold(path) : NULL;
}

void proc_run(void)
{
    set_path(&proc_running_path, arg_index(0));
}

const char *proc_running(void)
{
    return proc_running_path ? proc_running_path : "";
}

bool proc_has_launcher(void)
{
    return proc_launcher_path != NULL;
}

/* A program registers ITSELF -- its argv[0] -- as the one to return to. */
void proc_set_launcher(bool is_launcher)
{
    set_path(&proc_launcher_path, is_launcher ? proc_running_path : NULL);
}

void proc_cancel_launcher(void)
{
    set_path(&proc_launcher_path, NULL);
}

/* True when what is running now is the launcher: the chain ends here
 * rather than re-running it forever. */
bool proc_is_launcher(void)
{
    return proc_launcher_path && proc_running_path &&
           strcmp(proc_running_path, proc_launcher_path) == 0;
}

int16_t proc_get_exit_code(void)
{
    return proc_exit_code;
}

void proc_set_exit_code(int16_t code)
{
    proc_exit_code = code;
}

/* The code, then the stop -- the chain is the stop walk's to decide. */
void proc_exit(int16_t exit_code)
{
    proc_set_exit_code(exit_code);
    sys_stop();
}

/* A program stopped. Returns true when the launcher was asked for, meaning
 * the machine keeps running; false when the chain has ended and the caller
 * decides what a machine with nothing to run does. */
bool proc_stop(void)
{
    if (proc_exec_inflight())
    {
        /* A load is already on its way -- the exiting program asked for it,
         * or the user did. Do not clobber it with the launcher. */
        set_path(&proc_running_path, NULL);
        return true;
    }
    bool relaunch = !proc_is_launcher() && proc_has_launcher();
    set_path(&proc_running_path, NULL);
    if (!relaunch)
    {
        set_path(&proc_launcher_path, NULL);
        return false;
    }
    arg_clear();
    arg_append(proc_launcher_path);
    proc_exec_relaunch();
    return true;
}

/* op 0x08: the running program's argv, onto the xstack. */
bool proc_api_argv(void)
{
    return api_return_ax(arg_push_xstack());
}

/* op 0x09: replace the running program. The xstack holds the new argv;
 * argv[0] names the .rp6502. Committed once the argv parses -- a load error
 * surfaces on the console, because the program that asked is already gone. */
bool proc_api_exec(void)
{
    if (!arg_pull_xstack())
        return api_return_errno(API_EINVAL);
    if (!arg_index(0))
        return api_return_errno(API_EINVAL);
    proc_exec_start();
    return api_return_ax(0);
}
