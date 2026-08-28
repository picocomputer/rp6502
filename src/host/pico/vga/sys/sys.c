/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "rp6502_version.h"
#include "core/mem.h"
#include "vga/sys/ria.h"
#include "vga/sys/sys.h"
#include <hardware/flash.h>
#include <pico.h>
#include <pico/stdlib.h>
#include <string.h>

__in_flash("SYS_VERSION") static const char SYS_VERSION[] =
    "VGA " RP6502_VERSION;

__in_flash("sys_version") const char *sys_version(void)
{
    return SYS_VERSION;
}

static volatile bool sys_flash_pending;
static volatile uint16_t sys_flash_sector;

bool sys_flash_request(uint16_t sector_index)
{
    if ((uint32_t)sector_index >= PICO_FLASH_SIZE_BYTES / FLASH_SECTOR_SIZE)
        return false;
    sys_flash_sector = sector_index;
    sys_flash_pending = true;
    return true;
}

void sys_task(void)
{
    if (!sys_flash_pending)
        return;
    sys_flash_pending = false;

    /* This blocks every other task for tens of milliseconds, deliberately:
     * a sector write is rare, the RIA is the only thing that asks for one,
     * and video is core 1's plus core 0's ISRs, which keep running. */
    const uint32_t flash_offs = (uint32_t)sys_flash_sector * FLASH_SECTOR_SIZE;
    const uint8_t *src = (const uint8_t *)xram;

    flash_range_erase(flash_offs, FLASH_SECTOR_SIZE);
    flash_range_program(flash_offs, src, FLASH_SECTOR_SIZE);

    if (memcmp((const void *)(XIP_BASE + flash_offs), src, FLASH_SECTOR_SIZE) == 0)
        ria_ack();
    else
        ria_nak();
}
