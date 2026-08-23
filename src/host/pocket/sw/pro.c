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

#include "msc.h"
#include "pro.h"
#include "rom.h"

#include "core/api/api.h"
#include "core/api/arg.h"
#include "core/api/pro.h"
#include "host/pico/ria/main.h"

#include <stdio.h>
#include <string.h>

/* Short of the host's 256-byte field because this is static RAM. */
#define PRO_ARGV0_MAX 128

static char pro_argv0[PRO_ARGV0_MAX];
static char pro_exec_path[PRO_ARGV0_MAX];
/* pro_argv0 is the host's answer and does not change on an exec, so the
 * chain tracks argv[0] of each run instead. */
static char pro_running_path[PRO_ARGV0_MAX];
static char pro_launcher_path[PRO_ARGV0_MAX];
static bool pro_exec_pending;

/* Once per staged image, not once per run: asking is a blocking bridge
 * command and a run does not change the answer. Never on an exec, where
 * the argument buffer already holds what the outgoing program passed. */
void pro_restage(void)
{
    pro_exec_pending = false;
    /* A program the user picked from the menu supersedes the chain the
     * one before it was in. */
    pro_launcher_path[0] = '\0';
    pro_running_path[0] = '\0';
    arg_clear();
    if (msc_getfile(MSC_SLOT_ROM, pro_argv0, sizeof pro_argv0))
        arg_append(pro_argv0);
}

/* An exec staged its own image, so the chain's path is the one the
 * store holds; before any exec that is the host's answer. */
const char *pro_staged_path(void)
{
    return pro_running_path[0] ? pro_running_path : pro_argv0;
}

void pro_run(void)
{
    const char *argv0 = arg_index(0);
    if (argv0 && strlen(argv0) < sizeof pro_running_path)
        memcpy(pro_running_path, argv0, strlen(argv0) + 1);
    else
        pro_running_path[0] = '\0';
}

bool pro_has_launcher(void)
{
    return pro_launcher_path[0] != '\0';
}

void pro_set_launcher(bool is_launcher)
{
    if (is_launcher)
        memcpy(pro_launcher_path, pro_running_path,
               strlen(pro_running_path) + 1);
    else
        pro_launcher_path[0] = '\0';
}

bool pro_is_launcher(void)
{
    return pro_launcher_path[0] != '\0' &&
           strcmp(pro_running_path, pro_launcher_path) == 0;
}

void pro_cancel_launcher(void)
{
    pro_launcher_path[0] = '\0';
}

/* An exec the program asked for wins; otherwise it returns to the
 * launcher. The launcher's own exit ends the chain. */
void pro_stop(void)
{
    bool relaunch = !pro_exec_pending && pro_has_launcher() &&
                    !pro_is_launcher();
    pro_running_path[0] = '\0';
    if (!relaunch)
    {
        if (!pro_exec_pending)
            pro_launcher_path[0] = '\0';
        return;
    }
    memcpy(pro_exec_path, pro_launcher_path, strlen(pro_launcher_path) + 1);
    pro_exec_pending = true;
    arg_clear();
    arg_append(pro_exec_path);
}

bool pro_api_argv(void)
{
    return api_return_ax(arg_push_xstack());
}

bool pro_api_exec(void)
{
    if (!arg_pull_xstack())
        return api_return_errno(API_EINVAL);
    const char *path = arg_index(0);
    if (!path || strlen(path) >= sizeof pro_exec_path)
        return api_return_errno(API_EINVAL);
    memcpy(pro_exec_path, path, strlen(path) + 1);
    pro_exec_pending = true;
    /* Committed: errors surface on the console, because the program that
     * asked is already gone. */
    main_stop();
    return api_return_ax(0);
}

bool pro_exec_take(void)
{
    if (!pro_exec_pending)
        return false;
    pro_exec_pending = false;
    uint32_t len;
    if (!msc_stage_rom(pro_exec_path, &len))
    {
        printf("exec: no %s\n", pro_exec_path);
        return false;
    }
    if (!rom_load_staged(len))
    {
        printf("exec: bad image\n");
        return false;
    }
    return true;
}
