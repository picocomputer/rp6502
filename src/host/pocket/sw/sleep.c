/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The host's savestates are core/sys/sst.c's, made and loaded in the
 * blob window, SST_BLOB, with the machine held. The window is in the
 * staging store, past the cache, so the host and the firmware see the
 * same bytes. The host reads a save once it is told ok. A load is the
 * host's blob behind its OS header, and its request. A wake is a load
 * into a firmware that has just booted, and a load into a running
 * machine stops it first, so the stop closes its files the usual way. A
 * load that fails says "bad savestate" and leaves the 6502 in reset until
 * the core is loaded again.
 *
 * Where the Pocket's side of this comes from:
 * - 0x00A0 and 0x00A4 drive sleep and wake, and the host reads a save
 *   once it is told ok: Analogue's docs,
 *   https://www.analogue.co/developer/docs/openfpga/host-target-commands
 *   and .../openfpga/core-definition-files/core-json.
 * - A wake is a fresh core that is then loaded: the Spacewar core,
 *   https://github.com/spacemen3/PDP-1, Analogue's own example.
 * - What a load writes into the window: the Memories file on the card
 *   has a 596-byte header in front and a thumbnail behind (measured on
 *   the owner's Pocket). Spacewar takes the host's writes straight in at
 *   savestate_addr, which suggests the header does not come. Either way
 *   sst_find finds the blob.
 * - A save waits for no file command in flight: a community PlayStation
 *   core reports that the Pocket serves no file reads while it writes a
 *   Memory.
 */

#include "sleep.h"
#include "apf.h"
#include "fs.h"
#include "mmio.h"

#include "core/api/std.h"
#include "core/sys/debug_log.h"
#include "core/sys/sst.h"
#include "core/sys/sst_size.h"
#include "core/sys/sys.h"
#include "pocket_version.h"

#include <stdio.h>

_Static_assert(SST_TOTAL <= POCKET_SST_BYTES,
               "the savestate is larger than gen/version.py's SST_BYTES");

static bool sleep_refused;

bool sleep_waking(void)
{
    return sleep_refused || (SST_CTL & (SST_BLOB_SEEN | SST_LOAD_REQ));
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
 * changes. */
static bool sleep_load(void)
{
    const char *why = "no savestate in the window";
    uint32_t started = UPTIME_LO;
    uint32_t failed = fs_failed_count();
    const uint8_t *blob = sst_find(SST_BLOB, SST_BLOB_MAX);
    if (blob)
    {
        sleep_hold();
        why = sst_load(blob, SST_BLOB_MAX - (size_t)(blob - SST_BLOB),
                       NULL, 0);
    }
    if (why)
    {
        /* Said on the screen, because the debug log is off across a sleep:
         * the reason, where in the window the savestate was found, how
         * long the load took and how many file commands failed in it. */
        RP6502_LOG(sst, ERROR, "load: %s", why);
        printf("\nbad savestate: %s\nat %ld, %lu ms, %lu file errors\n",
               why, blob ? (long)(blob - SST_BLOB) : -1L,
               (unsigned long)((UPTIME_LO - started) / 1000u),
               (unsigned long)(fs_failed_count() - failed));
        sleep_refused = true;
        sys_stop();
    }
    else
        apf_refresh();
    MACH_HOLD = 0;
    SST_CTL = why ? SST_LOAD_ERR : SST_LOAD_OK;
    return !why;
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
    if (sleep_refused || !(ctl & (SST_BLOB_SEEN | SST_LOAD_REQ)))
        return false;
    if (sys_active())
    {
        sys_stop();
        return false;
    }
    if (!fs_idle() || !(ctl & SST_LOAD_REQ) || !(ctl & SST_WQ_EMPTY))
        return false;
    return sleep_load();
}
