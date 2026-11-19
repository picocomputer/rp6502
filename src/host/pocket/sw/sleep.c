/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The host's savestates are core/sys/sst.c's, made and loaded in the
 * blob window, SST_BLOB, with the machine held. The window is in the
 * staging store, past the cache, so the host and the firmware see the
 * same bytes. The host reads a save once it is told ok. A load is the
 * host's blob at the start of the window, and its request. A wake is a
 * load into a firmware that has just booted, and a load into a running
 * machine stops it first, so the stop closes its files the usual way. A
 * load that fails says "bad savestate" and leaves the machine stopped, as
 * a program that has ended does, until a ROM is started.
 *
 * Where the Pocket's side of this comes from:
 * - 0x00A0 and 0x00A4 drive sleep and wake, and the host reads a save
 *   once it is told ok: Analogue's docs,
 *   https://www.analogue.co/developer/docs/openfpga/host-target-commands
 *   and .../openfpga/core-definition-files/core-json.
 * - A wake is a fresh core that is then loaded: the Spacewar core,
 *   https://github.com/spacemen3/PDP-1, Analogue's own example.
 * - A load writes the blob at the start of the window, with nothing in
 *   front: measured on the owner's Pocket, and Spacewar takes the host's
 *   writes straight in at savestate_addr.
 * - The host serves no file command while its load request is
 *   unanswered: measured on the owner's Pocket.
 * - A save waits for no file command in flight: a community PlayStation
 *   core reports that the Pocket serves no file reads while it writes a
 *   Memory.
 */

#include "sleep.h"
#include "apf.h"
#include "fs.h"
#include "mmio.h"
#include "proc.h"

#include "core/api/std.h"
#include "core/sys/debug_log.h"
#include "core/sys/sst.h"
#include "core/sys/sys.h"
#include "pocket_version.h"

#include <stdio.h>

bool sleep_waking(void)
{
    return SST_CTL & (SST_BLOB_SEEN | SST_LOAD_REQ);
}

/* The machine stops on a clock its audio is quiet, and stays stopped
 * until it is let go. */
static void sleep_hold(void)
{
    MACH_HOLD = MACH_HOLD_ON;
    while (!(MACH_HOLD & MACH_HELD))
        ;
}

static void sleep_save(void)
{
    if (std_sst_refused())
    {
        SST_CTL = SST_SAVE_ERR;
        return;
    }
    sleep_hold();
    const char *why = sst_save(SST_BLOB, POCKET_SST_BYTES, 0);
    MACH_HOLD = 0;
    if (why)
        RP6502_LOG(sst, ERROR, "save: %s", why);
    SST_CTL = why ? SST_SAVE_ERR : SST_SAVE_OK;
}

/* The live input is taken again after a load, because the KEYB and GPAD
 * rows put back what was held at the save, and apf.c sends only what
 * changes. A load that fails may have put back part of a machine, a
 * launcher or an exec on its way among it, so those are dropped, the
 * machine is stopped, and its chain ends, which closes the files the rows
 * reopened. */
static void sleep_load(void)
{
    uint32_t started = UPTIME_LO;
    uint32_t failed = fs_failed_count();
    /* The host serves no file command while its load request is
     * unanswered, and the rows reopen files, so the request is answered
     * once the blob checks out and before the rows run, which do not
     * check it again. A row that refuses after that leaves the machine
     * stopped. */
    const char *why = sst_check(SST_BLOB, SST_BLOB_MAX, 0);
    SST_CTL = why ? SST_LOAD_ERR : SST_LOAD_OK;
    if (!why)
    {
        sleep_hold();
        why = sst_load_checked(SST_BLOB, NULL, 0);
    }
    if (why)
    {
        /* Said on the screen, because the debug log is off across a sleep:
         * the reason, how long the load took and how many file commands
         * failed in it. */
        RP6502_LOG(sst, ERROR, "load: %s", why);
        printf("\nbad savestate: %s\n%lu ms, %lu file errors\n", why,
               (unsigned long)((UPTIME_LO - started) / 1000u),
               (unsigned long)(fs_failed_count() - failed));
        /* The chain ends once: where an exec was on its way, here; where
         * the machine was left running, at its stop; and otherwise now. */
        const bool running = sys_running();
        const bool pending = proc_exec_inflight();
        proc_exec_drop();
        if (running)
            sys_stop();
        else if (!pending)
            proc_chain_end();
    }
    else
        apf_refresh();
    MACH_HOLD = 0;
}

bool sleep_task(void)
{
    uint32_t ctl = SST_CTL;
    if (ctl & SST_SAVE_REQ)
    {
        /* A save would write over the blob a load is waiting on. */
        if (sleep_waking())
            SST_CTL = SST_SAVE_ERR;
        else if (fs_idle())
            sleep_save();
    }
    if (!(ctl & (SST_BLOB_SEEN | SST_LOAD_REQ)))
        return false;
    if (sys_active())
    {
        sys_stop();
        return false;
    }
    if (!fs_idle() || !(ctl & SST_LOAD_REQ) || !(ctl & SST_WQ_EMPTY))
        return false;
    sleep_load();
    return true;
}
