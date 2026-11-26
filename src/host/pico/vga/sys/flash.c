/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/pix.h"
#include "core/sys/xram.h"
#include "vga/sys/flash.h"
#include "vga/sys/ria.h"
#include <hardware/flash.h>
#include <pico.h>
#include <pico/stdlib.h>
#include <string.h>

static volatile bool flash_pending;
static volatile uint16_t flash_page;

/* FLASH_PROGRAM is to be refused without the key (core/sys/pix.h), so that a
 * stray message on the PIX bus cannot erase flash. This comes in two
 * releases. The first, of 2026-11-23, has the RIA send the key and the VGA
 * clear it, and nothing checks it: a RIA from before that date sends no key,
 * and a VGA that checked would refuse its FLASH. A later release, once the
 * RIAs in use have had time to update, sets this to 1. */
#define FLASH_KEY_REQUIRED 0

static_assert(PIX_FLASH_KEY_ADDR == FLASH_PAGE_SIZE, "the key follows the page");

#if FLASH_KEY_REQUIRED
static bool flash_unlocked(void)
{
    const volatile uint32_t *key = (const volatile uint32_t *)&xram[PIX_FLASH_KEY_ADDR];
    for (uint32_t i = 0; i < PIX_FLASH_KEY_SIZE / sizeof(uint32_t); i++)
        if (key[i] != PIX_FLASH_KEY)
            return false;
    return true;
}
#endif

void flash_lock(void)
{
    memset((void *)xram, 0, PIX_FLASH_KEY_ADDR + PIX_FLASH_KEY_SIZE);
}

bool flash_program_request(uint16_t page)
{
#if FLASH_KEY_REQUIRED
    if (!flash_unlocked())
        return false;
#endif
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

    const uint32_t offs = (uint32_t)flash_page * FLASH_PAGE_SIZE;
    const uint8_t *dest = (const uint8_t *)(XIP_NOCACHE_NOALLOC_BASE + offs);
    const uint8_t *src = (const uint8_t *)xram;

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
