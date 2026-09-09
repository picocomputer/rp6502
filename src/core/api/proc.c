/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * See proc.h.
 */

#include "core/api/proc.h"
#include "core/api/api.h"
#include "core/api/arg.h"
#include "core/sys/sys.h"
#include "osal/dir.h"

#include <string.h>

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

const char *proc_launcher(void)
{
    return proc_launcher_path ? proc_launcher_path : "";
}

void proc_restore_paths(const char *running, const char *launcher)
{
    set_path(&proc_running_path, running);
    set_path(&proc_launcher_path, launcher);
}

void proc_set_launcher(bool is_launcher)
{
    set_path(&proc_launcher_path, is_launcher ? proc_running_path : NULL);
}

void proc_cancel_launcher(void)
{
    set_path(&proc_launcher_path, NULL);
}

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

void proc_exit(int16_t exit_code)
{
    proc_set_exit_code(exit_code);
    sys_stop();
}

bool proc_stop(void)
{
    if (proc_exec_inflight())
    {
        /* A load the exiting program or the user already asked for is on its
         * way, so the launcher must not be scheduled over it. */
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

bool proc_api_argv(void)
{
    return api_return_ax(arg_push_xstack());
}

/* op 0x09. The xstack holds the new argv, whose argv[0] names the .rp6502.
 * The op succeeds as soon as the argv parses, because by the time a load can
 * fail the program that asked for it is gone; the error goes to the
 * console. */
bool proc_api_exec(void)
{
    if (!arg_pull_xstack())
        return api_return_errno(API_EINVAL);
    if (!arg_index(0))
        return api_return_errno(API_EINVAL);
    proc_exec_start();
    return api_return_ax(0);
}
