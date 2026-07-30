/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_API_UNI_H_
#define _RIA_API_UNI_H_

/* The OEM code page conversions, in place of FatFs's ffunicode.c. The
 * three entry points below belong to FatFs and keep its names, because
 * ff.c calls them from a dozen places and oem.c calls two of them; they
 * are declared in fatfs/ff.h and repeated here only for a build that
 * has no FatFs at all.
 *
 * The tables are data, not code, and the platform says where they live:
 * uni_word is the one thing a port has to write. A machine with flash
 * links them in; the Pocket keeps them in its staging store and reads
 * them a word at a time.
 */

#include <stdbool.h>
#include <stdint.h>

/* One 16-bit word of the table image, by index. Written per platform. */
uint16_t uni_word(uint32_t index);

/* Read the image's header and cache what the lookups need. False when
 * the magic is wrong, which means the tables did not load — a port that
 * fetches them from a file should say so and stop rather than run on
 * and turn every accented filename into mojibake.
 *
 * A platform that links the tables in need not call this; the lookups
 * do it themselves the first time they are asked. */
bool uni_init(void);

uint16_t ff_oem2uni(uint16_t oem, uint16_t cp);
uint16_t ff_uni2oem(uint32_t uni, uint16_t cp);
uint32_t ff_wtoupper(uint32_t uni);

#endif /* _RIA_API_UNI_H_ */
