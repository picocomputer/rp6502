/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The software machines' loader: it loads the records of a .rp6502 into sram[]
 * and xram[]. The pump that reads them is pump.c's, shared with the other
 * loaders.
 */

#include "osal/fs.h"
#include "core/rom/rom.h"
#include "core/sys/buf1k.h"
#include "core/ria/regs.h"
#include "core/wdc/sram.h"
#include "core/sys/xram.h"
#include "core/str/str.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* A load bypasses the bus, so it has to land the bytes where the bus would
 * have left them. $FF00-$FFF9 is dropped, because the firmware's
 * ria_write_buf drops it too. The $FFFA-$FFFF vectors go to sram[], which
 * shadows every address for the debug views, and to regs[], which is what the
 * RIA answers a read of that window from. */
static void rom_deposit(const rom_record_t *rec, const uint8_t *buf)
{
    if (rec->addr > 0xFFFF)
    {
        for (uint32_t i = 0; i < rec->len; i++)
            xram[rec->addr - 0x10000 + i] = buf[i]; /* volatile, so not memcpy */
        return;
    }
    for (uint32_t i = 0; i < rec->len; i++)
    {
        uint32_t a = rec->addr + i;
        if (a < 0xFF00 || a >= 0xFFFA)
            sram[a] = buf[i];
        if (a >= 0xFFFA && a <= 0xFFFF)
            regs[a & 0x1F] = buf[i];
    }
}

/* The console message for the reason that the last rom_load failed. */
static const char *rom_error;

#define ROM_SST(X) X(SKIP, rom_error, "read right after the load that sets it")
SST_TABLE(rom, ROM_SST)

const char *rom_load_error(void)
{
    return rom_error;
}

bool rom_load(const char *path)
{
    rom_assets_reset();
    api_errno err;
    rom_pump_t pump;
    /* An installed ":name" is opened as its host file here, because
     * fs_rom_open on these hosts has no list of installed ROMs. */
    int fd = rom_alias_open(path, &err);
    if (fd < 0 || !rom_pump_open_fd(&pump, fd, &err))
    {
        rom_error = api_errno_str(err);
        return false;
    }
    rom_record_t rec;
    rom_pump_result r;
    while ((r = rom_pump_next(&pump, &rec, &err)) != ROM_PUMP_EOF)
    {
        if (r == ROM_PUMP_SKIP)
            continue;
        if (r == ROM_PUMP_ERROR)
        {
            rom_error = api_errno_str(err);
            rom_pump_close(&pump);
            return false;
        }
        rom_deposit(&rec, buf1k);
    }
    if (!rom_pump_complete(&pump))
    {
        rom_error = S(STR_ERR_ROM_NO_RESET_VECTOR);
        rom_pump_close(&pump);
        return false;
    }
    rom_asset_adopt(pump.fd, pump.assets_start);
    return true;
}
