/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/emu/pro.h"
#include "emu/emu/msc.h"
#include "emu/host/host.h"
#include "emu/sys/mem.h"
#include "emu/sys/cpu.h"
#include "ria/api/api.h"
#include "ria/api/arg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Launcher chain (firmware pro.c): pro_running_path is argv[0] of the program
 * running now; pro_launcher_path is the ROM to re-run when a program exits.
 * pro_exit_code holds the last exit code for the EXIT_CODE attribute. */
static char pro_running_path[MSC_MAX_PATH];
static char pro_launcher_path[MSC_MAX_PATH];
static int16_t pro_exit_code;

/* Pending exec (op 0x09): the new program loads at the frame boundary rather
 * than mid-tick, so the master clock and the partially-run frame stay
 * consistent. pro_exec() captures the ROM path and stops the current program;
 * the frame loop commits it via pro_take_exec(). */
static bool exec_pending;
static char exec_path[MSC_MAX_PATH];

void pro_init(void)
{
    exec_pending = false;
}

void pro_exec(const char *rom_path)
{
    snprintf(exec_path, sizeof(exec_path), "%s", rom_path);
    exec_pending = true;
    cpu_set_halted(true); /* stop the current program; the tick loop exits */
}

const char *pro_take_exec(void)
{
    if (!exec_pending)
        return NULL;
    exec_pending = false;
    return exec_path;
}

bool pro_exec_pending(void)
{
    return exec_pending;
}

/* Seed the initially loaded program's argv (firmware rom_load_argv/rom_exec).
 * argv[0] is the program's own path in 6502 form so it can re-exec itself: a
 * drive path or an installed ":name" is used verbatim; a host path maps back
 * through realpath. Empty args are kept, like the monitor's LOAD. */
bool pro_set_argv(const char *rom, int argc, char *const *args)
{
    char abs[MSC_MAX_PATH], msc[MSC_MAX_PATH];
    const char *argv0 = rom;
    if (!msc_has_drive_prefix(rom) && rom[0] != ':' && fs_realpath(rom, abs, sizeof(abs)))
    {
        msc_from_host(abs, msc, sizeof(msc));
        argv0 = msc;
    }
    /* Length-guard each string: arg_append's uint16 math trusts
     * monitor-capped tokens, but host input is unbounded. */
    arg_clear();
    bool ok = strlen(argv0) < XSTACK_SIZE && arg_append(argv0);
    for (int i = 0; ok && i < argc; i++)
        ok = strlen(args[i]) < XSTACK_SIZE && arg_append(args[i]);
    if (!ok)
        arg_clear(); /* no partial argv; the caller decides severity */
    pro_run(); /* the initial program is now what's running */
    return ok;
}

/* Snapshot argv[0] of the program now starting (firmware pro_run), so
 * pro_running_path always names what is running. */
void pro_run(void)
{
    const char *argv0 = arg_index(0);
    snprintf(pro_running_path, sizeof pro_running_path, "%s", argv0 ? argv0 : "");
}

/* LAUNCHER attribute get: is a launcher armed? */
bool pro_has_launcher(void)
{
    return pro_launcher_path[0] != '\0';
}

/* LAUNCHER attribute set: a program registers ITSELF (its argv[0]) as the
 * launcher to re-run after each child exits, or clears the chain. */
void pro_set_launcher(bool is_launcher)
{
    if (is_launcher)
        snprintf(pro_launcher_path, sizeof pro_launcher_path, "%s", pro_running_path);
    else
        pro_launcher_path[0] = '\0';
}

/* True when the program running now is the launcher itself — the chain ends
 * here rather than re-running it forever. */
bool pro_is_launcher(void)
{
    return pro_launcher_path[0] != '\0' &&
           strcmp(pro_running_path, pro_launcher_path) == 0;
}

int16_t pro_get_exit_code(void)
{
    return pro_exit_code;
}

/* Program EXIT (op 0xFF), mirroring firmware pro_stop. Records the exit code
 * and, if a launcher is armed and the exiting program is not itself the
 * launcher, schedules a re-exec of the launcher ROM and returns true (the
 * machine keeps running). Otherwise the chain ends and it returns false, so the
 * caller halts. */
bool pro_exit(int16_t exit_code)
{
    pro_exit_code = exit_code;
    bool relaunch = !pro_is_launcher() && pro_has_launcher();
    pro_running_path[0] = '\0';
    if (!relaunch)
    {
        pro_launcher_path[0] = '\0';
        return false;
    }
    /* Build the relaunch argv from a copy: arg_append overwrites the argv buffer,
     * and the launcher path must survive to seed pro_running_path on reload. */
    char path[MSC_MAX_PATH];
    snprintf(path, sizeof path, "%s", pro_launcher_path);
    arg_clear();
    arg_append(path);
    pro_exec(path);
    return true;
}

/* op 0x08: read the running program's argv onto the xstack. */
bool pro_api_argv(void)
{
    return api_return_ax(arg_push_xstack());
}

/* op 0x09: replace the running program. The xstack holds the new argv buffer;
 * argv[0] names the .rp6502 to load. Committed once validated — load errors
 * surface on reload, matching the firmware. */
bool pro_api_exec(void)
{
    if (!arg_pull_xstack())
        return api_return_errno(API_EINVAL);
    /* argv[0] (an MSC0:/overlay/host name) is resolved by the loader at the
     * frame boundary. */
    pro_exec(arg_index(0));
    return api_return_ax(0);
}
