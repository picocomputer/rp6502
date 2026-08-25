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
 * which is pro_exec_request, and whether one is already on its way, which
 * is pro_exec_inflight.
 */

#include "core/api/pro.h"
#include "core/api/api.h"
#include "core/api/arg.h"
#include "host.h" /* PRO_PATH_MAX: how long a path this machine can hold */

#include <stdio.h>
#include <string.h>

/* argv[0] of the program running now, and of the one to come back to. */
static char pro_running_path[PRO_PATH_MAX];
static char pro_launcher_path[PRO_PATH_MAX];
static int16_t pro_exit_code;

void pro_run(void)
{
    const char *argv0 = arg_index(0);
    snprintf(pro_running_path, sizeof pro_running_path, "%s", argv0 ? argv0 : "");
}

const char *pro_running(void)
{
    return pro_running_path;
}

bool pro_has_launcher(void)
{
    return pro_launcher_path[0] != '\0';
}

/* A program registers ITSELF -- its argv[0] -- as the one to return to. */
void pro_set_launcher(bool is_launcher)
{
    if (is_launcher)
        snprintf(pro_launcher_path, sizeof pro_launcher_path, "%s", pro_running_path);
    else
        pro_launcher_path[0] = '\0';
}

void pro_cancel_launcher(void)
{
    pro_launcher_path[0] = '\0';
}

/* True when what is running now is the launcher: the chain ends here
 * rather than re-running it forever. */
bool pro_is_launcher(void)
{
    return pro_launcher_path[0] != '\0' &&
           strcmp(pro_running_path, pro_launcher_path) == 0;
}

int16_t pro_get_exit_code(void)
{
    return pro_exit_code;
}

void pro_set_exit_code(int16_t code)
{
    pro_exit_code = code;
}

/* A program stopped. Returns true when the launcher was asked for, meaning
 * the machine keeps running; false when the chain has ended and the caller
 * decides what a machine with nothing to run does. */
bool pro_stop(void)
{
    if (pro_exec_inflight())
    {
        /* A load is already on its way -- the exiting program asked for it,
         * or the user did. Do not clobber it with the launcher. */
        pro_running_path[0] = '\0';
        return true;
    }
    bool relaunch = !pro_is_launcher() && pro_has_launcher();
    pro_running_path[0] = '\0';
    if (!relaunch)
    {
        pro_launcher_path[0] = '\0';
        return false;
    }
    /* From a copy: arg_append overwrites the argv buffer the launcher path
     * would otherwise be read out of. */
    char path[PRO_PATH_MAX];
    snprintf(path, sizeof path, "%s", pro_launcher_path);
    arg_clear();
    arg_append(path);
    pro_exec_relaunch(path);
    return true;
}

/* op 0x08: the running program's argv, onto the xstack. */
bool pro_api_argv(void)
{
    return api_return_ax(arg_push_xstack());
}

/* op 0x09: replace the running program. The xstack holds the new argv;
 * argv[0] names the .rp6502. Committed once the argv parses -- a load error
 * surfaces on the console, because the program that asked is already gone. */
bool pro_api_exec(void)
{
    if (!arg_pull_xstack())
        return api_return_errno(API_EINVAL);
    const char *path = arg_index(0);
    if (!path)
        return api_return_errno(API_EINVAL);
    pro_exec_start(path);
    return api_return_ax(0);
}
