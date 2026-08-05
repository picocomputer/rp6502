/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What the host actually does with data slots, measured instead of
 * assumed. Built only when RP6502_POCKET_PROBE is on, and then the
 * machine loads no ROM: the screen is the instrument and a program would
 * scribble over it.
 *
 * Two things are being asked. The first is where the data table puts a
 * slot's pair — the bench models it indexed by id, the firmware's own
 * comment says the host decides and scans for the id instead, and
 * pocket_bridge reads a fixed word 17 that is only the ROM's size if the
 * bench is right. The second is whether the host sends the update event
 * at all, and for which slots: APF's command 0x008A names the slot and
 * its size outright, was never wired to anything until now, and is the
 * only thing that could tell a running core that the user picked a
 * different ROM.
 *
 * A photograph of the screen is the result, so the dump is sized to fit
 * one: sixty-four words four to a line, then only what is not zero.
 */

#include "mmio.h"
#include "msc.h"
#include "probe.h"

#include <stdio.h>

void probe_dump(void)
{
    printf("\nslot probe\n");
    for (uint32_t w = 0; w < 64; w += 4)
        printf("%03u %08x %08x %08x %08x\n", (unsigned)w,
               (unsigned)msc_dt(w), (unsigned)msc_dt(w + 1),
               (unsigned)msc_dt(w + 2), (unsigned)msc_dt(w + 3));
    /* The table is 256 words. Everything past the first sixty-four is
     * only worth a line if the host put something there. */
    for (uint32_t w = 64; w < 256; w++)
    {
        uint32_t v = msc_dt(w);
        if (v)
            printf("%03u %08x\n", (unsigned)w, (unsigned)v);
    }
    printf("set=%08x upd=%08x len=%08x\n", (unsigned)MMIO_SLOT,
           (unsigned)MMIO_UPD_ID, (unsigned)MMIO_UPD_LEN);
}

void probe_task(void)
{
    /* Bit 16 is the flag, because slot 0 is a real id and its update
     * would otherwise read as nothing having happened. */
    uint32_t id = MMIO_UPD_ID;
    if (id & 0x10000u)
    {
        uint32_t len = MMIO_UPD_LEN;
        MMIO_UPD_ID = 0;
        printf("upd slot=%u len=%u\n", (unsigned)(id & 0xFFFFu),
               (unsigned)len);
    }
    uint32_t set = MMIO_SLOT;
    if (set)
    {
        MMIO_SLOT = 0;
        printf("set len=%u\n", (unsigned)set);
    }
}
