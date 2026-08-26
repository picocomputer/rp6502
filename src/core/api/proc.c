/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The launcher chain: which program is running, which one to return to
 * when it exits, and what it exited with. Every machine ran the same rules
 * -- a program registers itself, its children come back to it, and the
 * chain ends when the launcher itself stops -- and each had written them
 * out. What actually differs is how a machine starts the next program,
 * which is proc_exec_request, and whether one is already on its way, which
 * is proc_exec_inflight.
 */

#include "core/api/proc.h"
#include "core/api/api.h"
#include "core/api/arg.h"
#include "host.h" /* PROC_PATH_MAX: how long a path this machine can hold */

#include <stdio.h>
#include <string.h>

/* argv[0] of the program running now, and of the one to come back to. */
static char proc_running_path[PROC_PATH_MAX];
static char proc_launcher_path[PROC_PATH_MAX];
static int16_t proc_exit_code;

void proc_run(void)
{
    const char *argv0 = arg_index(0);
    snprintf(proc_running_path, sizeof proc_running_path, "%s", argv0 ? argv0 : "");
}

const char *proc_running(void)
{
    return proc_running_path;
}

bool proc_has_launcher(void)
{
    return proc_launcher_path[0] != '\0';
}

/* A program registers ITSELF -- its argv[0] -- as the one to return to. */
void proc_set_launcher(bool is_launcher)
{
    if (is_launcher)
        snprintf(proc_launcher_path, sizeof proc_launcher_path, "%s", proc_running_path);
    else
        proc_launcher_path[0] = '\0';
}

void proc_cancel_launcher(void)
{
    proc_launcher_path[0] = '\0';
}

/* True when what is running now is the launcher: the chain ends here
 * rather than re-running it forever. */
bool proc_is_launcher(void)
{
    return proc_launcher_path[0] != '\0' &&
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

/* A program stopped. Returns true when the launcher was asked for, meaning
 * the machine keeps running; false when the chain has ended and the caller
 * decides what a machine with nothing to run does. */
bool proc_stop(void)
{
    if (proc_exec_inflight())
    {
        /* A load is already on its way -- the exiting program asked for it,
         * or the user did. Do not clobber it with the launcher. */
        proc_running_path[0] = '\0';
        return true;
    }
    bool relaunch = !proc_is_launcher() && proc_has_launcher();
    proc_running_path[0] = '\0';
    if (!relaunch)
    {
        proc_launcher_path[0] = '\0';
        return false;
    }
    /* From a copy: arg_append overwrites the argv buffer the launcher path
     * would otherwise be read out of. */
    char path[PROC_PATH_MAX];
    snprintf(path, sizeof path, "%s", proc_launcher_path);
    arg_clear();
    arg_append(path);
    proc_exec_relaunch(path);
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
    const char *path = arg_index(0);
    if (!path)
        return api_return_errno(API_EINVAL);
    proc_exec_start(path);
    return api_return_ax(0);
}
