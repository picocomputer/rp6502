/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The firmware's whole part in sleep and savestates, which is smaller
 * than it looks like it should be.
 *
 * It has nothing to do with making a blob. The engine stops the machine
 * to read it, and this soft CPU is part of the machine — halted at its
 * debug port for the duration — so no code here runs while one is being
 * made and none is needed: the host is answered in fabric.
 *
 * Restoring is where there is work, and it arrives as an event rather
 * than a boot. The engine puts every memory and every register back and
 * then lets this core go at the instruction it was stopped in front of,
 * so what runs afterwards is the firmware the blob brought, mid-whatever
 * it was doing, with a bit set saying it has been somewhere.
 *
 * What it puts back is the fabric no blob carries: the parts that are
 * written and never read, and the parts that belong to the host rather
 * than to the machine. The 6502 is held stopped until this is finished,
 * which is what makes clearing the bit the last thing here.
 *
 * At boot the bit to ask about is the other one. The host writes the
 * blob in as ordinary bridge writes before it asks for the load, so a
 * boot that finds anything in the window knows a restore is coming and
 * declines to start the ROM under it.
 */

#include "sst.h"

#include "aud.h"
#include "font.h"
#include "main.h"
#include "mmio.h"
#include "msc.h"

#include "ria/api/tim.h"

bool sst_pending(void)
{
    return (SST_CTL & SST_BLOB_SEEN) != 0;
}

void sst_task(void)
{
    if (!(SST_CTL & SST_RESTORED))
        return;

    /* Refused, and nothing was written: this is still the session it
     * was, so there is nothing to fix up and every fixup would be
     * wrong -- the clock re-based mid-run, the canvas repainted under
     * a live program. The engine still holds the machine's release
     * hostage to the ack, so the ack is all this path does. */
    if (SST_CTL & SST_RESTORE_ERR)
    {
        SST_CTL = SST_RESTORED;
        return;
    }

    /* Write-only fabric, all of it, and all of it derived from
     * something the blob did carry: the code page, the pointer each
     * audio engine is at and the block it reads. */
    font_restore();
    aud_restore();

    /* The host's slot-to-path bindings are a session's, and a wake is a
     * new session. */
    msc_restore();

    /* The microsecond counter starts again from zero across a
     * reconfigure while the base taken from it came out of the blob, so
     * every reading after would be the sleep's whole length out. Taking
     * the base again from the host's reading is also the only way the
     * machine learns how long it was gone; a clock a program had set
     * for itself does not survive that, and the host's is the one that
     * is right. */
    tim_init();

    /* The host re-announces its slots on a wake and the loop reads a
     * change in either announcement as the user picking a new program.
     * Neither is news: what is staged is what this machine is already
     * running. */
    main_restored();

    SST_CTL = SST_RESTORED;
}
