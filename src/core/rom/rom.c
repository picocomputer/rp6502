/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The software machines' loader: pump the records into ram[]/xram[]. The
 * pump is rom_pump.c's, shared by every machine; the null drive the seam
 * resolves ":name" through is alias.c's.
 */

#include "core/log.h"
#include "core/api/fs.h"
#include "core/str/path.h"
#include "core/rom/rom.h"
#include "core/rom/rom_rec.h"
#include "host/os.h"
#include "core/mem/mem.h"
#include "core/str/str.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* ------------------------------------------------------------------ */
/* This machine's loader: pump the records into ram[]/xram[]           */
/* ------------------------------------------------------------------ */

/* Deposit one record's bytes. A load never writes the RIA register window:
 * $FF00-$FFF9 is skipped (the firmware's ria_write_buf does the same over
 * the bus), and the $FFFA-$FFFF vectors land in the register cells too --
 * a load bypasses the bus, so ram[] keeps the shadow every reader uses and
 * regs[] gets the copy the RIA would have taken. */
static void rom_deposit(const rom_rec_t *rec, const uint8_t *buf)
{
    if (rec->addr > 0xFFFF)
    {
        for (uint32_t i = 0; i < rec->len; i++)
            xram[rec->addr - 0x10000 + i] = buf[i]; /* volatile: no memcpy */
        return;
    }
    for (uint32_t i = 0; i < rec->len; i++)
    {
        uint32_t a = rec->addr + i;
        if (a < 0xFF00 || a >= 0xFFFA)
            ram[a] = buf[i];
        if (a >= 0xFFFA && a <= 0xFFFF)
            regs[a & 0x1F] = buf[i];
    }
}

bool rom_load(const char *path)
{
    /* The alias map is the loader's alone: an installed ":name" becomes its
     * backing file here, and the seam below never sees a colon on this
     * machine. */
    char host[HOST_MAX_PATH];
    if (!rom_resolve(path, host, sizeof(host)))
    {
        log_error("cannot resolve ROM '%s'", path);
        return false;
    }
    rom_assets_reset(); /* forget the previous ROM (the MSC0: drive persists) */
    api_errno err;
    rom_pump_t pump;
    if (!rom_pump_open(&pump, host, &err))
    {
        log_error("cannot load ROM '%s'", path);
        return false;
    }
    static uint8_t buf[ROM_REC_MAX];
    rom_rec_t rec;
    rom_pump_result r;
    while ((r = rom_pump_next(&pump, buf, &rec, &err)) != ROM_PUMP_EOF)
    {
        if (r == ROM_PUMP_SKIP)
            continue;
        if (r == ROM_PUMP_ERROR)
        {
            log_error("bad ROM record in '%s'", path);
            rom_pump_close(&pump);
            return false;
        }
        rom_deposit(&rec, buf);
    }
    if (!rom_pump_complete(&pump))
    {
        log_error("ROM has no reset vector ($FFFC/$FFFD)");
        rom_pump_close(&pump);
        return false;
    }
    /* The descriptor and the directory offset become the ROM: drive's: an
     * asset open scans the file for the entry and reads it on demand, so the
     * bytes never enter RAM. */
    rom_asset_adopt(pump.fd, pump.assets_start);
    return true;
}
