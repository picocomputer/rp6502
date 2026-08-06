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
 * into a filesystem; here the same thing is Open File to bind a slot,
 * Slot Read to pull the image into the window the host stages into, and
 * the ordinary loader on top. The image lands where the host would have
 * put it, so its bundled assets arrive with it and the ROM: driver
 * needs to know nothing about how it got there.
 *
 * The load happens after the machine has stopped rather than inside the
 * syscall, because stopping closes the descriptors the outgoing program
 * left open and one of those slots is what the read needs.
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
static bool pro_exec_pending;

/* Asked once per staged image rather than once per run, because asking
 * is a blocking bridge command and a run does not change the answer.
 * Never on an exec: the argument buffer already holds what the outgoing
 * program passed, and that is the whole point of passing it. */
void pro_restage(void)
{
    pro_exec_pending = false;
    arg_clear();
    if (msc_getfile(MSC_SLOT_ROM, pro_argv0, sizeof pro_argv0))
        arg_append(pro_argv0);
    /* The answer landed in the window rom.c reads the image through. */
    rom_win_invalidate();
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
