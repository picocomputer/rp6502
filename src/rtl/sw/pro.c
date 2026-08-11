/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "msc.h"
#include "pro.h"
#include "rom.h"

#include "ria/api/api.h"
#include "ria/api/arg.h"
#include "ria/api/pro.h"
#include "ria/main.h"

#include <stdio.h>
#include <string.h>

#define PRO_ARGV0_MAX 128

static char pro_argv0[PRO_ARGV0_MAX];
static char pro_exec_path[PRO_ARGV0_MAX];
static char pro_running_path[PRO_ARGV0_MAX];
static char pro_launcher_path[PRO_ARGV0_MAX];
static bool pro_exec_pending;

void pro_restage(void)
{
    pro_exec_pending = false;
    pro_launcher_path[0] = '\0';
    pro_running_path[0] = '\0';
    arg_clear();
    if (msc_getfile(MSC_SLOT_ROM, pro_argv0, sizeof pro_argv0))
        arg_append(pro_argv0);
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
        printf("exec: %s\n", pro_exec_path);
        return false;
    }
    if (!rom_load_staged(len))
    {
        printf("exec: bad image\n");
        return false;
    }
    return true;
}
