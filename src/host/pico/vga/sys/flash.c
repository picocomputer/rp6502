/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/xram.h"
#include "vga/sys/flash.h"
#include "vga/sys/ria.h"
#include <hardware/flash.h>
#include <pico.h>
#include <pico/stdlib.h>
#include <string.h>

static volatile bool flash_pending;
static volatile uint16_t flash_sector;

bool flash_request(uint16_t sector_index)
{
    if ((uint32_t)sector_index >= PICO_FLASH_SIZE_BYTES / FLASH_SECTOR_SIZE)
        return false;
    flash_sector = sector_index;
    flash_pending = true;
    return true;
}

void flash_task(void)
{
    if (!flash_pending)
        return;
    flash_pending = false;

    /* This blocks every other task for tens of milliseconds, deliberately:
     * a sector write is rare, the RIA is the only thing that asks for one,
     * and video is core 1's plus core 0's ISRs, which keep running. */
    const uint32_t flash_offs = (uint32_t)flash_sector * FLASH_SECTOR_SIZE;
    const uint8_t *src = (const uint8_t *)xram;

    flash_range_erase(flash_offs, FLASH_SECTOR_SIZE);
    flash_range_program(flash_offs, src, FLASH_SECTOR_SIZE);

    if (memcmp((const void *)(XIP_BASE + flash_offs), src, FLASH_SECTOR_SIZE) == 0)
        ria_ack();
    else
        ria_nak();
}
