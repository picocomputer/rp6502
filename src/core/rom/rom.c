/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The software machines' loader: it loads the records of a .rp6502 into sram[]
 * and xram[]. The pump that reads them is pump.c's, shared with the other
 * loaders.
 */

#include "core/sys/com.h"
#include "osal/fs.h"
#include "core/rom/rom.h"
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

bool rom_load(const char *path)
{
    /* An installed ":name" becomes its backing file here, because fs_rom_open
     * on these hosts has no store of its own to look one up in. */
    const char *host = rom_alias_resolve(path);
    rom_assets_reset();
    api_errno err;
    rom_pump_t pump;
    static uint8_t buf[ROM_RECORD_MAX];
    if (!rom_pump_open(&pump, host, buf, &err))
    {
        com_printf("cannot load ROM '%s'\n", path);
        return false;
    }
    rom_record_t rec;
    rom_pump_result r;
    while ((r = rom_pump_next(&pump, buf, &rec, &err)) != ROM_PUMP_EOF)
    {
        if (r == ROM_PUMP_SKIP)
            continue;
        if (r == ROM_PUMP_ERROR)
        {
            com_printf("bad ROM record in '%s'\n", path);
            rom_pump_close(&pump);
            return false;
        }
        rom_deposit(&rec, buf);
    }
    if (!rom_pump_complete(&pump))
    {
        com_printf("ROM has no reset vector ($FFFC/$FFFD)\n");
        rom_pump_close(&pump);
        return false;
    }
    rom_asset_adopt(pump.fd, pump.assets_start);
    return true;
}
