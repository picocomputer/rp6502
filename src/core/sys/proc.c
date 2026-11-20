/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/sys.h"
#include "core/sys/proc.h"
#include "core/rom/rom.h"
#include "core/api/proc.h"
#include "core/api/arg.h"
#include "core/ria/regs.h"
#include "core/wdc/sram.h"
#include "core/sys/xram.h"
#include "core/com/com.h"
#include "core/hid/paste.h"
#include "core/str/str.h"
#include "osal/dir.h"
#include "osal/fs.h"
#include "osal/os.h"
#include <stdlib.h>
#include <string.h>

static bool queued;

/* The line that the last proc_boot printed, for a host that also shows it
 * somewhere other than the console. */
static const char *boot_error = "";

/* proc_exec_task loads what argv[0] names, so an exec on its way with no
 * argv[0] is refused. The ARG row loads before this one. */
static bool proc_queued_valid(sst_cursor_t *c)
{
    if (queued && !arg_index(0))
        return sst_refuse(c, "an exec is on its way with no program");
    return true;
}

#define PROC_SST(X)                                                       \
    X(BOOL, queued)                                                       \
    X(VALIDATE, proc_queued_valid)                                        \
    X(SKIP, boot_error, "read right after the boot that sets it")
SST_TABLE(proc, PROC_SST)

void proc_exec_init(void)
{
    queued = false;
}

/* No exec is on its way, so proc_exec_task loads nothing. The chain is
 * CHAN's to reset, and what ends a chain in other drivers is their own
 * reset's job. */
void proc_reset(void)
{
    queued = false;
}

void proc_exec_request(void)
{
    queued = true;
    sys_stop();
}

/* Measured in size_t, because arg_append computes with uint16_t and the host
 * does not limit these lengths. argv[0] is a path, so it is limited to
 * API_PATH_MAX rather than to the xstack size. */
static bool argv_fits(const char *argv0, int argc, char *const *args)
{
    size_t len = strlen(argv0);
    if (len > API_PATH_MAX)
        return false;
    /* An offset for each string, the zero pair ending the table, then the
     * strings. */
    size_t size = ((size_t)argc + 2) * 2 + len + 1;
    for (int i = 0; i < argc && size <= XSTACK_SIZE; i++)
        size += strlen(args[i]) + 1;
    return size <= XSTACK_SIZE;
}

/* A ':name' is an installed ROM that rom_load resolves, not a path. */
static char *argv0_path(const char *rom)
{
    return (rom[0] == ':') ? NULL : os_dir_realpath(rom);
}

bool proc_argv_fits(const char *rom, int argc, char *const *args)
{
    char *abs = argv0_path(rom);
    bool ok = argv_fits(abs ? abs : rom, argc, args);
    free(abs);
    return ok;
}

/* An exec leaves argv[0] as the program wrote it, so it is made absolute
 * here, before the new program can chdir, to record where the ROM was. */
static bool argv0_absolute(void)
{
    char *abs = argv0_path(arg_index(0));
    bool ok = !abs || (strlen(abs) <= API_PATH_MAX && arg_replace(0, abs));
    free(abs);
    return ok;
}

bool proc_set_argv(const char *rom, int argc, char *const *args)
{
    char *abs = argv0_path(rom);
    const char *argv0 = abs ? abs : rom;
    arg_clear();
    bool ok = argv_fits(argv0, argc, args);
    if (ok)
    {
        arg_append(argv0);
        for (int i = 0; i < argc; i++)
            arg_append(args[i]);
    }
    free(abs);
    proc_run();
    return ok;
}

const char *proc_boot_error(void)
{
    return boot_error;
}

/* A failed exec ends its chain before the error prints, so the error comes
 * out after the soft reset. */
static bool proc_boot_failed(const char *why, int argc)
{
    boot_error = why;
    if (argc < 0)
        proc_exec_failed();
    com_clean_line();
    com_printf("%s", why);
    return false;
}

void proc_start_by_name(void)
{
    proc_cancel_launcher();
    sys_stop_now();
    /* Cleared after that stop and not before, because the stop reaches
     * proc_stop, which reads proc_exec_inflight to decide whether the
     * launcher comes back. An exec it leaves queued ends with the chain. */
    if (queued)
    {
        queued = false;
        proc_chain_end();
    }
    paste_cancel();
}

bool proc_boot(const char *rom, int argc, char *const *args, unsigned flags)
{
    /* Either stops what was running before the load writes over it. */
    if (argc >= 0)
        proc_start_by_name();
    else
        sys_stop_now();
    queued = false;
    boot_error = "";
    if (argc < 0 ? !argv0_absolute() : !proc_argv_fits(rom, argc, args))
        return proc_boot_failed(S(STR_ERR_ROM_ARGV_OVERFLOW), argc);
    if (argc < 0)
        rom = arg_index(0);
    if (flags & PROC_REFILL)
    {
        sram_init();
        xram_init();
    }
    if (!rom_load(rom))
        return proc_boot_failed(rom_load_error(), argc);
    if (argc >= 0)
        proc_set_argv(rom, argc, args);
    fs_save_start();
    sys_run();
    return true;
}

void proc_exec_start(void)
{
    proc_exec_request();
}

void proc_exec_relaunch(void)
{
    proc_exec_request();
}

void proc_exec_break(void)
{
    if (!queued)
        return;
    queued = false;
    proc_chain_end();
}

void proc_shutdown(void)
{
    proc_break();
    sys_commit();
}

bool proc_exec_inflight(void)
{
    return queued;
}

/* The session's end waits for the program's output to go out at the
 * console line's pace, and the program has exited once that is done. */
bool proc_exited(void)
{
    return !sys_running() && !proc_exec_inflight() && !com_ending();
}

void proc_exec_task(void)
{
    if (!queued)
        return;
    /* argv[0] is still where the request left it, because the only thing that
     * rewrites argv on a stop is proc_stop arming a launcher relaunch, and it
     * does not do that while an exec is in flight. */
    proc_boot(arg_index(0), -1, NULL, 0);
}
