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
#include "core/wdc/resb.h"
#include "osal/dir.h"
#include "osal/os.h"
#include <stdlib.h>
#include <string.h>

static bool queued;

void proc_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    sst_put_bool(c, queued);
    sst_put_i16(c, proc_get_exit_code());
    sst_put_str(c, proc_running(), PROC_PATH_SLOT);
    sst_put_str(c, proc_launcher(), PROC_PATH_SLOT);
    sst_put(c, arg_data(), arg_bytes());
}

bool proc_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    bool pending = sst_get_bool(c);
    int16_t code = sst_get_i16(c);
    static char running[PROC_PATH_SLOT], launcher[PROC_PATH_SLOT];
    sst_get_str(c, running, sizeof running);
    sst_get_str(c, launcher, sizeof launcher);
    static uint8_t argv[XSTACK_SIZE];
    sst_get(c, argv, sizeof argv);
    if (!sst_ok(c))
        return false;
    queued = pending;
    proc_set_exit_code(code);
    proc_restore_paths(running, launcher);
    arg_set_data(argv);
    return true;
}

void proc_exec_init(void)
{
    queued = false;
}

void proc_exec_request(void)
{
    queued = true;
    resb_assert();
}

bool proc_set_argv(const char *rom, int argc, char *const *args)
{
    /* A ':name' is an installed ROM that rom_load resolves, not a path. */
    char *abs = (rom[0] == ':') ? NULL : os_dir_realpath(rom);
    const char *argv0 = abs ? abs : rom;
    /* Every string is measured first, because arg_append computes with
     * uint16_t and these come from a host that bounds nothing. argv[0] is a
     * path, so it is held to what a path may be rather than to what the
     * xstack happens to hold. */
    arg_clear();
    bool ok = strlen(argv0) <= API_PATH_MAX && arg_append(argv0);
    for (int i = 0; ok && i < argc; i++)
        ok = strlen(args[i]) < XSTACK_SIZE && arg_append(args[i]);
    if (!ok)
        arg_clear();
    free(abs);
    proc_run();
    return ok;
}

bool proc_boot(const char *rom, int argc, char *const *args, unsigned flags)
{
    sys_stop_now(); /* before the load writes over what it was running on */
    /* Cleared after that stop and not before, because the walk it performs
     * reaches proc_stop, which reads proc_exec_inflight to decide whether the
     * launcher comes back. Whatever the outgoing program had queued goes with
     * it, since a start asked for by name is not its child. */
    queued = false;
    if (flags & PROC_REFILL)
    {
        sram_init();
        xram_init();
    }
    if (!rom_load(rom)) /* rom_load has already said why on the console */
        return false;
    if (argc >= 0)
        proc_set_argv(rom, argc, args);
    if (flags & PROC_UNCHAIN)
        proc_set_launcher(false);
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

bool proc_exec_inflight(void)
{
    return queued;
}

bool proc_exited(void)
{
    return !resb_running() && !proc_exec_inflight();
}

void proc_exec_task(void)
{
    if (!queued)
        return;
    /* argv[0] is still where the request left it, because the only thing that
     * rewrites argv on a stop is proc_stop arming a launcher relaunch, and it
     * does not do that while an exec is in flight. */
    if (!proc_boot(arg_index(0), -1, NULL, 0))
        proc_set_exit_code(1);
}
