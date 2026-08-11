/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sst.h"

#include "aud.h"
#include "font.h"
#include "main.h"
#include "mmio.h"
#include "msc.h"
#include "rom.h"
#include "vga.h"
#include "vid.h"

#include "ria/api/tim.h"

#include <pico/time.h>

#include <stdio.h>

static void sst_log_restore(uint32_t ctl)
{
    uint64_t us = time_us_64();
    printf("sst: restore ctl=%02x mtime=%u:%u\n", (unsigned)(ctl & 0xFFu),
           (unsigned)(us >> 32), (unsigned)us);
    printf("sst: canvas=%u vsync=%u prog=%08x page=%u\n",
           (unsigned)vga_get_canvas(), (unsigned)vga_vsync_scanline(),
           (unsigned)vid_prog_word_get(), (unsigned)font_get_code_page());
    printf("sst: slot=%u upd=%u boot=%u/%u/%u\n", (unsigned)MMIO_SLOT,
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

    if (ctl & SST_SAVED)
    {
        uint64_t us = time_us_64();
        printf("sst: saved mtime=%u:%u canvas=%u aud=%04x/%04x\n",
               (unsigned)(us >> 32), (unsigned)us,
               (unsigned)vga_get_canvas(), (unsigned)aud_psg_at_get(),
               (unsigned)aud_opl_at_get());
        aud_log_psg("saved");
        aud_log_opl("saved");
        msc_log();
        SST_CTL = SST_SAVED;
    }

    if (!(ctl & SST_RESTORED))
        return;
    sst_log_restore(ctl);

    if (ctl & SST_RESTORE_ERR)
    {
        printf("sst: refused, staging the rom instead\n");
        SST_CTL = SST_RESTORED;
        main_wake_failed();
        return;
    }

    font_restore();
    aud_restore();

    vga_restore();
    vid_restore();

    msc_restore();

    tim_init();
    {
        uint64_t us = time_us_64();
        printf("sst: released mtime=%u:%u\n", (unsigned)(us >> 32),
               (unsigned)us);
        rom_log();
        msc_log();
    }

    main_restored();

    SST_CTL = SST_RESTORED;
}
