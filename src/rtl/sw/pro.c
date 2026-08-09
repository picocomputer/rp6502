/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * argv and exec. The core is handed a staged image and never told what
 * it was called, so argv[0] has to be asked for — Get File on the ROM
 * slot — and it is kept exactly as the host spells it, absolute path and
 * all. That is what lets a program open itself, and what lets exec hand
 * a path back.
 *
 * Exec is the machine staging its own next program. The RIA reaches
 * into a filesystem; here the same thing is Open File to bind the ROM
 * slot, Slot Reads to pull the whole image to where the host stages one,
 * and the ordinary loader on top. The image lands where the host would
 * have put it, so its bundled assets arrive with it and the ROM: driver
 * needs to know nothing about how it got there.
 *
 * The load happens after the machine has stopped rather than inside the
 * syscall, because stopping closes the descriptors the outgoing program
 * left open and one of those slots is what the read needs.
 *
 * The launcher chain is the RIA's, with this machine's staging in place
 * of its filesystem. A program registers itself as the launcher and
 * every program that exits afterwards comes back to it — which on a
 * Pocket is the whole of what a break used to be for: there is no
 * monitor to drop into, so alt-f4 either has a launcher to return to or
 * does nothing at all.
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

/* The host's answer verbatim, which is an absolute path. Short of the
 * host's 256-byte field because this is static RAM, and long enough
 * that only a name nobody could pick would not fit. */
#define PRO_ARGV0_MAX 128

static char pro_argv0[PRO_ARGV0_MAX];
static char pro_exec_path[PRO_ARGV0_MAX];
/* What is running now, and what to come back to when it ends. The
 * staged name above is the host's answer and does not change on an
 * exec, so the chain tracks argv[0] of each run instead. */
static char pro_running_path[PRO_ARGV0_MAX];
static char pro_launcher_path[PRO_ARGV0_MAX];
static bool pro_exec_pending;

/* Asked once per staged image rather than once per run, because asking
 * is a blocking bridge command and a run does not change the answer.
 * Never on an exec: the argument buffer already holds what the outgoing
 * program passed, and that is the whole point of passing it. */
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

/* The machine is starting: whatever argv holds now is what runs. */
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

/* The program stopped. An exec it asked for wins — it named its own
 * successor — and otherwise the launcher is what it comes back to. The
 * launcher's own exit ends the chain, because there is nothing above it
 * to return to. */
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
    /* Committed. Errors surface on the console the way rom.c's do on the
     * RIA, because the program that asked is already gone. */
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
