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
 * Loading a memory does not boot the machine. It was measured: the same
 * digest of the staging store at boot and again after the restore, with
 * the data table still naming the old slot length. The host resets
 * nothing, restreams nothing and re-announces nothing -- it writes the
 * blob into a live machine and asks for the load.
 *
 * A real sleep is a different thing and has not been tested. It cuts
 * power, so its wake is a core launch, and a core launch does fill slot
 * 0 before anything runs. The boot-time check below is for that case;
 * on hardware it has only ever read zero.
 */

#include "sst.h"

#include "aud.h"
#include "com.h"
#include "font.h"
#include "log.h"
#include "main.h"
#include "mmio.h"
#include "msc.h"
#include "proc.h"
#include "rom.h"
#include "vga.h"
#include "vid.h"

#include "core/api/tim.h"

#include "host.h"

#include <stdio.h>
#include <string.h>

/* Everything about a wake this side is not certain of, said once, at
 * the one moment all of it is knowable. The device is the only place
 * these questions can be put -- Analogue documents none of it -- and a
 * hardware pass is expensive, so this asks the whole list in one go
 * rather than one question per bitstream.
 *
 * Once per restore and never in a loop: a running program owns the
 * console and this must not become weather. */
static void sst_log_restore(uint32_t ctl)
{
    uint64_t us = host_clock_us();
    LOG_SAY("sst: restore ctl=%02x mtime=%u:%u\n", (unsigned)(ctl & 0xFFu),
           (unsigned)(us >> 32), (unsigned)us);
    LOG_SAY("sst: canvas=%u vsync=%u prog=%08x page=%u\n",
            (unsigned)vga_get_canvas(), (unsigned)vga_vsync_scanline(),
           (unsigned)vid_prog_word_get(), (unsigned)font_get_code_page());
    LOG_SAY("sst: slot=%u upd=%u boot=%u/%u/%u\n", (unsigned)MMIO_SLOT,
           (unsigned)(MMIO_UPD_N & 0xFFu), (unsigned)main_boot_wake,
           (unsigned)main_boot_slot, (unsigned)main_boot_upd);
}

bool sst_pending(void)
{
    return (SST_CTL & SST_BLOB_SEEN) != 0;
}

void sst_task(void)
{
    uint32_t ctl = SST_CTL;
    if (!(ctl & SST_RESTORED))
        return;
    /* Before anything downstream can fail, so a log that reaches here
     * and no further says where. */
    LOG_SAY("sst: restored\n");
    sst_log_restore(ctl);

    /* Refused, and nothing was written: this is still the session it
     * was, so there is nothing to fix up and every fixup would be
     * wrong -- the clock re-based mid-run, the canvas repainted under
     * a live program. The engine still holds the machine's release
     * hostage to the ack, so the ack is all this path does. */
    if (ctl & SST_RESTORE_ERR)
    {
        printf("sst: refused, staging the rom instead\n");
        SST_CTL = SST_RESTORED;
        /* Acked first, because staging can take the better part of a
         * second and the ack is the cheaper thing to owe. Nothing
         * documents a deadline on 0x00A4 and 726 ms has been accepted
         * without complaint, so this is an ordering preference and not
         * a constraint.
         *
         * Only a boot that declined to stage has anything to do here:
         * it has nothing at all, so the ROM the host announced is
         * staged after all. A refusal into a running machine leaves
         * that session exactly as it was. */
        main_wake_failed();
        return;
    }

    /* Write-only fabric, all of it, and all of it derived from
     * something the blob did carry: the code page, the pointer each
     * audio engine is at and the block it reads. */
    font_restore();
    aud_restore();

    /* The raster's registers: the canvas, the vsync line and the
     * terminal's window. The blob carries the scanline table and the
     * cells they describe, and carries the firmware's own shadows of
     * all three, but the registers themselves are fabric and a wake
     * brings them back at power-on. */
    vga_restore();
    vid_restore();

    /* The host's slot-to-path bindings are a session's, and a wake is a
     * new session. */
    msc_restore();

    /* The staging store is the board's and no blob carries it, and the
     * device has been asked: loading a memory does not restore slot 0
     * and does not re-announce it. So the store holds whatever the
     * session that was running put there, while rom.c's directory
     * offsets came back in the blob describing another program, and
     * every ROM: asset the restored session opens would be read out of
     * the wrong file.
     *
     * A core launch fills slot 0 completely before anything runs. A
     * resume has to do the same for itself, here, while the 6502 is
     * still held -- which is also the only place it can be done without
     * a program watching its own assets change underneath it. */
    {
        /* By name, because a length is not an identity: two images of
         * the same size would read as the same program. The name is the
         * one the exec or the boot staged, carried in the blob, against
         * what the host says slot 0 is bound to now. A load into a
         * machine already running that program -- nothing reset,
         * nothing restreamed -- has the store right already and is owed
         * nothing.
         *
         * Relative against absolute: msc_stage_rom opens under the
         * assets folder, so the host spells back what this side asked
         * for with that in front. */
        const char *want = proc_staged_path();
        char bound[128];
        uint32_t len = 0;
        bool same = false;
        if (want && *want && msc_getfile(MSC_SLOT_ROM, bound, sizeof bound))
        {
            const char *at = bound;
            if (*want != '/'
                && !strncmp(bound, MSC_ASSETS_PATH, sizeof MSC_ASSETS_PATH - 1))
                at += sizeof MSC_ASSETS_PATH - 1;
            same = !strcmp(at, want);
            LOG_SAY("rom: want '%s' bound '%s'%s\n", want, bound,
                    same ? " same" : "");
        }
        if (!same && (!want || !*want))
            printf("rom: no path to stage\n");
        else if (!same && !msc_stage_rom(want, &len))
            printf("rom: stage '%s' failed\n", want);
        else if (!same)
        {
            /* Nothing re-parses the image -- rom_load_staged would
             * rewrite the 6502 memory the blob just restored -- so the
             * length is the only check left, and a file that changed on
             * the card since the memory was made is worth saying. */
            if (len != rom_staged_len())
                printf("rom: staged %u, session had %u\n", (unsigned)len,
                       (unsigned)rom_staged_len());
        }
    }

    /* The microsecond counter starts again from zero across a
     * reconfigure while the base taken from it came out of the blob, so
     * every reading after would be the sleep's whole length out. Taking
     * the base again from the host's reading is also the only way the
     * machine learns how long it was gone; a clock a program had set
     * for itself does not survive that, and the host's is the one that
     * is right. */
    tim_init();
    LOG_SAY("sst: released mtime=%u:%u\n",
            (unsigned)(host_clock_us() >> 32), (unsigned)host_clock_us());
    msc_log();

    /* The host re-announces its slots on a wake and the loop reads a
     * change in either announcement as the user picking a new program.
     * Neither is news: what is staged is what this machine is already
     * running. */
    main_restored();

    SST_CTL = SST_RESTORED;
}
