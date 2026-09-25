/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * When a savestate load succeeds, the engine (sst_engine.sv) resumes the
 * soft CPU at the instruction where it was halted when the blob was made,
 * so the firmware that runs afterwards is the one in the blob.
 * SST_RESTORED then reads set, and the 6502 stays in reset until
 * SST_RESTORED is written to SST_CTL, which clears the bit, so wake_task
 * makes that write only after the state the blob does not contain has
 * been put back.
 */

#include "wake.h"

#include "aud.h"
#include "com.h"
#include "font.h"
#include "main.h"
#include "mmio.h"
#include "fs.h"
#include "proc.h"
#include "rom.h"
#include "vga.h"
#include "vid.h"

#include "core/api/tim.h"
#include "core/sys/debug_log.h"

#include <string.h>

bool wake_pending(void)
{
    return (SST_CTL & SST_BLOB_SEEN) != 0;
}

void wake_task(void)
{
    uint32_t ctl = SST_CTL;
    if (!(ctl & SST_RESTORED))
        return;

    /* The engine checks the whole blob before it writes any of it, so a
     * failed load has written nothing and there is nothing to put back. */
    if (ctl & SST_RESTORE_ERR)
    {
        RP6502_LOG(sst, ERROR, "refused, staging the rom instead");
        SST_CTL = SST_RESTORED;
        main_wake_failed();
        return;
    }

    font_restore();
    aud_restore();

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

    SST_CTL = SST_RESTORED;
}
