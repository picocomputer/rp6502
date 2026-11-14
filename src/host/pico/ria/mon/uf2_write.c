/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ria/mon/uf2_write.h"
#include "core/str/str.h"
#include "core/sys/debug_log.h"
#include "core/com/com.h"
#include "core/sys/sys.h"
#include "core/sys/timer.h"
#include "ria/sys/mbuf.h"
#include <boot/uf2.h>
#include <hardware/flash.h>
#include <hardware/watchdog.h>
#include <pico/stdio.h>
#include <stdio.h>
#include <string.h>

bool uf2_read_block(FIL *fil)
{
    UINT br;
    return f_read(fil, mbuf, 512, &br) == FR_OK && br == 512;
}

void uf2_block_pages(uint32_t *first, uint32_t *count)
{
    const struct uf2_block *b = UF2_BLOCK;
    uint32_t base = b->target_addr - XIP_BASE;
    *first = base / FLASH_PAGE_SIZE;
    uint32_t last = b->payload_size ? (base + b->payload_size - 1) / FLASH_PAGE_SIZE : *first;
    *count = last - *first + 1;
}

void uf2_build_page(uint32_t page)
{
    const struct uf2_block *b = UF2_BLOCK;
    uint32_t base = b->target_addr - XIP_BASE;
    uint32_t page_base = page * FLASH_PAGE_SIZE;
    uint32_t from = base > page_base ? base : page_base;
    uint32_t to = base + b->payload_size;
    if (to > page_base + FLASH_PAGE_SIZE)
        to = page_base + FLASH_PAGE_SIZE;
    memset(UF2_PAGE, 0xFF, FLASH_PAGE_SIZE);
    memcpy(UF2_PAGE + (from - page_base), b->data + (from - base), to - from);
}

void uf2_progress(uint32_t done, uint32_t total, int *last)
{
    int pct = (int)((uint64_t)done * 100 / total);
    if (pct != *last)
    {
        *last = pct;
        printf(STR_UF2_FLASHING, pct);
        stdio_flush();
    }
}

void uf2_lock_up(void)
{
    for (;;)
        sys_pump();
}

static void __attribute__((noreturn)) uf2_write_failed(void)
{
    // The message is the format, because a literal "%s" would be in flash.
    // The terminal is not fed while flashing, so it cannot tell a clean line.
    com_putchar('\r');
    com_putchar('\n');
    printf(STR_UF2_FLASH_FAILED);
    uf2_lock_up();
}

/* From sys_set_flashing on, the main loop never runs again, and a blocking
 * wait pumps only the TASK_ALWAYS tasks. */
void uf2_write_ria(FIL *fil, uint32_t num_blocks)
{
    sys_set_flashing();
    int last_percent = -1;
    for (uint32_t block = 0; block < num_blocks; block++)
    {
        if (!uf2_read_block(fil))
        {
            RP6502_LOG(uf2, ERROR, "read failed at block %lu", (unsigned long)block);
            uf2_write_failed();
        }
        uint32_t page, count;
        uf2_block_pages(&page, &count);
        for (; count; count--, page++)
        {
            uf2_build_page(page);
            uint32_t offs = page * FLASH_PAGE_SIZE;
            const uint8_t *dest = (const uint8_t *)(XIP_NOCACHE_NOALLOC_BASE + offs);
            for (uint32_t i = 0; i < FLASH_PAGE_SIZE; i++)
                if (dest[i] != 0xFF)
                {
                    flash_range_erase(offs & ~(FLASH_SECTOR_SIZE - 1), FLASH_SECTOR_SIZE);
                    break;
                }
            flash_range_program(offs, UF2_PAGE, FLASH_PAGE_SIZE);
            if (memcmp(dest, UF2_PAGE, FLASH_PAGE_SIZE))
            {
                RP6502_LOG(uf2, ERROR, "page 0x%08lX did not take", (unsigned long)offs);
                uf2_write_failed();
            }
        }
        uf2_progress(block + 1, num_blocks, &last_percent);
    }
    putchar('\n');
    stdio_flush();
    // Time for the last line to leave over telnet.
    timer_deadline_t drained = timer_in_ms(1000);
    while (!timer_passed(drained))
        sys_pump();
    watchdog_reboot(0, 0, 0);
    for (;;)
        tight_loop_contents();
}
