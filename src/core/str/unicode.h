/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_STR_UNICODE_H_
#define _CORE_STR_UNICODE_H_

/* The OEM code page conversions, in place of FatFs's ffunicode.c. The
 * three entry points below belong to FatFs and keep its names and its
 * types, because ff.c calls them from a dozen places and oem.c calls two
 * of them; they are declared in fatfs/ff.h and repeated here only for a
 * build that has no FatFs at all.
 *
 * The tables are data, not code, and the platform says where they live:
 * unicode_word is the one thing a port has to write. A machine with flash
 * links them in; the Pocket keeps them in its staging store and reads
 * them a word at a time.
 */

#include <stdbool.h>
#include <stdint.h>

/* One 16-bit word of the table image, by index. Written per platform. */
uint16_t unicode_word(uint32_t index);

/* Read the image's header and cache what the lookups need. False when
 * the magic is wrong, which means the tables did not load — a port that
 * fetches them from a file should say so and stop rather than run on
 * and turn every accented filename into mojibake.
 *
 * A platform that links the tables in need not call this; the lookups
 * do it themselves the first time they are asked. */
bool unicode_init(void);

/* True when the tables carry this code page. */
bool unicode_has_page(uint16_t cp);

/* Only where FatFs is not. ff.h declares these itself, in types it picks
 * per platform — uint16_t/uint32_t on the C99 branch, windows.h's WORD and
 * DWORD wherever _WIN32 is defined — and DWORD is not uint32_t there. So
 * ff.h is the authority whenever a translation unit has it, and this is
 * for one that does not. */
#ifndef FF_DEFINED
uint16_t ff_oem2uni(uint16_t oem, uint16_t cp);
uint16_t ff_uni2oem(uint32_t uni, uint16_t cp);
uint32_t ff_wtoupper(uint32_t uni);
#endif

/* The UTF-8 codec, against an explicit code page. oem.c wraps these
 * with the page it is currently holding; a platform with no oem.c calls
 * them directly. A code point with no OEM character becomes '?' and a
 * malformed sequence becomes one '?' — never an ASCII byte the input did
 * not contain, because these decode untrusted host filenames. */
unsigned char unicode_from_codepoint(uint32_t cp, uint16_t page);
unsigned char unicode_from_utf8_next(const char **p, uint16_t page);
int unicode_to_utf8_char(unsigned char b, uint16_t page, char *dst);

/* This driver's row in a machine's driver list; see core/sys/driver.h. */
#define UNICODE_DRIVER DRIVER(unicode_init, nul_task, nul_task, nul_run, nul_stop, nul_break, nul_config, nul_config)

#endif /* _CORE_STR_UNICODE_H_ */
