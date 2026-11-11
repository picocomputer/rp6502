/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_MON_UF2_WRITE_H_
#define _RIA_MON_UF2_WRITE_H_

/* What runs while the RIA rewrites its own flash, and the block helpers the
 * VGA's update shares with it. The file is on RIA_RAM_SOURCES.
 */

#include <fatfs/ff.h>
#include <stdbool.h>
#include <stdint.h>

/* The block last read sits at the start of mbuf, and the page built from it
 * right after. */
#define UF2_BLOCK ((const struct uf2_block *)mbuf)
#define UF2_PAGE (mbuf + 512)

bool uf2_read_block(FIL *fil);
void uf2_block_pages(uint32_t *first, uint32_t *count);
void uf2_build_page(uint32_t page);

/* Prints the percentage of blocks done when it has changed since *last. */
void uf2_progress(uint32_t done, uint32_t total, int *last);

/* After a failed update: the consoles go on draining, so the error can be
 * read, and nothing else happens. Recovery is BOOTSEL. */
void __attribute__((noreturn)) uf2_lock_up(void);

/* Rewrites the RIA's flash from fil, positioned at the image's first block.
 * It starts flashing, and ends in a reboot or, on any failure, a lock-up. */
void __attribute__((noreturn)) uf2_write_ria(FIL *fil, uint32_t num_blocks);

#endif /* _RIA_MON_UF2_WRITE_H_ */
