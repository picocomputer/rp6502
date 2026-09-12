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
static volatile uint16_t flash_page;

bool flash_program_request(uint16_t page)
{
    if ((uint32_t)page >= PICO_FLASH_SIZE_BYTES / FLASH_PAGE_SIZE)
        return false;
    flash_page = page;
    flash_pending = true;
    return true;
}

void flash_task(void)
{
    if (!flash_pending)
        return;
    flash_pending = false;

    /* This blocks every other task, deliberately: the RIA is the only thing
     * that asks, and video is core 1's plus core 0's ISRs, which keep
     * running. */
    const uint32_t offs = (uint32_t)flash_page * FLASH_PAGE_SIZE;
    const uint8_t *dest = (const uint8_t *)(XIP_NOCACHE_NOALLOC_BASE + offs);
    const uint8_t *src = (const uint8_t *)xram;

    /* Programming only clears bits, so a page that still reads as erased takes
     * new data without disturbing the rest of its sector. One that does not is
     * holding old data, and the whole sector has to go. */
    for (uint32_t i = 0; i < FLASH_PAGE_SIZE; i++)
        if (dest[i] != 0xFF)
        {
            flash_range_erase(offs & ~(FLASH_SECTOR_SIZE - 1), FLASH_SECTOR_SIZE);
            break;
        }

    flash_range_program(offs, src, FLASH_PAGE_SIZE);

    if (memcmp(dest, src, FLASH_PAGE_SIZE) == 0)
        ria_ack();
    else
        ria_nak();
}
