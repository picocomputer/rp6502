/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * A wake is the host's blob and its load command, in either order, and
 * the first of them shows in SST_CTL. The program is stopped from then on,
 * because the restore replaces the machine's memory but not a file the
 * program creates or truncates before the load.
 *
 * When the load succeeds, the engine (sst_engine.sv) resumes the soft CPU
 * at the instruction where it was halted when the blob was made, so the
 * firmware that runs afterwards is the one in the blob. SST_RESTORED then
 * reads set, and the 6502 stays in reset until SST_RESTORED is written to
 * SST_CTL, which clears the bit, so wake_task makes that write only after
 * the state the blob does not contain has been put back.
 *
 * A wake that does not resume is one whose blob the engine refuses or one
 * whose load never comes. The console then says which, and the ROM is
 * staged and started as a boot does.
 */

#include "wake.h"

#include "main.h"
#include "mmio.h"
#include "fs.h"
#include "proc.h"
#include "rom.h"
#include "vga.h"
#include "vid.h"

#include "core/api/clk.h"
#include "core/aud/bel.h"
#include "core/api/tim.h"
#include "core/sys/debug_log.h"
#include "host/host.h"

#include "core/sys/sys.h"

#include <stdio.h>
#include <string.h>

/* The host writes a blob at 3.4 MB/s or faster and core_bridge_cmd gives
 * any of its commands 0.9 s, so a blob with no word written for 2 s and no
 * load asked for has been left. A load that comes later still finds it,
 * because dropping the blob clears only SST_BLOB_SEEN. */
#define WAKE_LOAD_US 2000000u

/* How long the console's line about a wake stays up before the ROM
 * starts. */
#define WAKE_SHOW_US 2000000u

static uint64_t wake_wrote;
static uint64_t wake_shown;

bool wake_pending(void)
{
    if (SST_CTL & (SST_BLOB_SEEN | SST_RESTORED))
        return true;
    if (wake_shown && (int64_t)(host_clock_us() - wake_shown) >= 0)
        wake_shown = 0;
    return wake_shown != 0;
}

static void wake_failed(const char *why, const char *when)
{
    RP6502_LOG(sst, ERROR, "%s%s", why, when);
    printf("\nwake: %s%s\n", why, when);
    wake_shown = host_clock_us() + WAKE_SHOW_US;
    sys_stop();
    main_restage();
}

void wake_task(void)
{
    uint32_t ctl = SST_CTL;
    if (!(ctl & SST_RESTORED))
    {
        if (!(ctl & SST_BLOB_SEEN))
            return;
        sys_stop();
        if (!(ctl & SST_BLOB_UNASKED))
            return;
        if (ctl & SST_BLOB_WRITING)
        {
            SST_CTL = SST_BLOB_WRITING;
            wake_wrote = host_clock_us();
        }
        else if (host_clock_us() - wake_wrote >= WAKE_LOAD_US)
        {
            SST_CTL = SST_BLOB_SEEN;
            wake_failed("no load came", "");
        }
        return;
    }

    /* The engine checks the whole blob before it writes any of it, so a
     * failed load has written nothing and there is nothing to put back. */
    if (ctl & SST_RESTORE_ERR)
    {
        static const char *const why[8] = {
            [SST_WHY_NO_BLOB] = "no header",
            [SST_WHY_VERSION] = "version differs",
            [SST_WHY_SIZE] = "length differs",
            [SST_WHY_BUILD] = "build differs",
            [SST_WHY_SUM] = "checksum differs",
            [SST_WHY_TRAILER] = "no trailer",
        };
        SST_CTL = SST_RESTORED;
        wake_failed(why[SST_WHY(ctl)],
                    ctl & SST_LOAD_EARLY ? " (asked in reset)" : "");
        return;
    }

    vga_restore();
    vid_restore();

    fs_restore();
    /* The staging store, which holds the file bound to slot 0, is not in
     * the blob, while the asset directory offset restored from the blob
     * describes the program the blob was saved from, so that program's
     * file is staged again here unless slot 0 is already bound to it or
     * its path is empty. The file is staged before SST_RESTORED is
     * written, because fs_rom_open fails whenever CPU_RESB reads 1, and
     * CPU_RESB reads 0 while the engine holds the 6502 in reset. */
    {
        const char *want = proc_staged_path();
        char bound[API_PATH_MAX + 1];
        bool same = want && *want
                    && fs_getfile(FS_SLOT_ROM, bound, sizeof bound)
                    && !strcmp(fs_strip_drive(want), bound);
        if (!same && (!want || !*want))
            RP6502_LOG(rom, ERROR, "no path to stage");
        else if (!same)
        {
            /* The image is not loaded with rom_load_fd, because that
             * would write its records over the 6502 memory the blob
             * restored, so its length is the only check that the file on
             * the card is the one the blob was saved with. */
            uint32_t had = fs_rom_staged_len();
            api_errno err;
            fs_std_close(FS_DESC_ROM, &err);
            if (fs_rom_open(want, FS_RD, &err) < 0)
                RP6502_LOG(rom, ERROR, "stage '%s' failed", want);
            else if (fs_rom_staged_len() != had)
                RP6502_LOG(rom, INFO, "staged %u, session had %u",
                           (unsigned)fs_rom_staged_len(), (unsigned)had);
        }
    }

    tim_init();
    fs_log();

    main_restored();

    /* The machine has been parked since the soft CPU resumed at the saved
     * mtime, so the bell's deadlines and the program's clock move by that
     * long, and the audio and the 6502 meet them as they would have. */
    uint32_t parked = (uint32_t)host_clock_us() - SST_SAVED_US;
    bel_wake(parked);
    clk_wake(parked);
    SST_CTL = SST_RESTORED;
}
