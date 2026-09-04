/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * This machine's .rp6502 deposit: the pump is core's, reading the staged
 * image through the ROM descriptor, and the bytes land in the fabric --
 * SRAM under $FF00 and above $FFF9, the register cells for the vectors,
 * XRAM_WIN above. Same rules as every loader: a load never writes
 * $FF00-$FFF9, and both reset vector bytes must arrive or the image is
 * rejected.
 */

#include "fs.h"
#include "mmio.h"
#include "rom.h"
#include "core/rom/rom.h"

#include <stdio.h>

static void rom_deposit(const rom_record_t *rec, const uint8_t *buf)
{
    for (uint32_t i = 0; i < rec->len; i++)
    {
        uint32_t a = rec->addr + i;
        uint8_t b = buf[i];
        if (a > 0xFFFF)
            XRAM_WIN[a - 0x10000] = b;
        else if (a < 0xFF00 || a >= 0xFFFA)
            SRAM[a] = b;
        if (a >= 0xFFFA && a <= 0xFFFF)
            REGS_WIN[a & 0x1F] = b;
    }
}

bool rom_load_fd(int fd)
{
    api_errno err;
    rom_pump_t pump;
    static uint8_t buf[ROM_RECORD_MAX];
    if (!rom_pump_open_fd(&pump, fd, buf, &err))
        return false;
    rom_record_t rec;
    rom_pump_result r;
    while ((r = rom_pump_next(&pump, buf, &rec, &err)) != ROM_PUMP_EOF)
    {
        if (r == ROM_PUMP_SKIP)
            continue;
        if (r == ROM_PUMP_ERROR)
        {
            rom_pump_close(&pump);
            return false;
        }
        rom_deposit(&rec, buf);
    }
    if (!rom_pump_complete(&pump))
    {
        rom_pump_close(&pump);
        return false;
    }
    rom_asset_adopt(pump.fd, pump.assets_start);
    return true;
}

bool rom_load(const char *path)
{
    rom_assets_reset();
    api_errno err;
    int fd = fs_rom_open(path, FS_RD, &err);
    if (fd < 0)
        return false;
    return rom_load_fd(fd);
}
