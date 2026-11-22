/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_STR_UNICODE_H_
#define _CORE_STR_UNICODE_H_

/* The OEM code page conversions, in place of FatFs's ffunicode.c. The three
 * ff_ entry points below keep FatFs's names and types because ff.c calls
 * them. */

#include <stdbool.h>
#include <stdint.h>

bool unicode_has_page(uint16_t cp);

/* Only where FatFs is not. ff.h declares these itself, in types it picks per
 * platform: uint16_t and uint32_t on its C99 branch, but windows.h's WORD and
 * DWORD wherever _WIN32 is defined, and that DWORD is unsigned long rather
 * than uint32_t. So ff.h is the authority whenever a translation unit has it,
 * and this is for one that does not. */
#ifndef FF_DEFINED
uint16_t ff_oem2uni(uint16_t oem, uint16_t cp);
uint16_t ff_uni2oem(uint32_t uni, uint16_t cp);
uint32_t ff_wtoupper(uint32_t uni);
#endif

/* The UTF-8 codec, against an explicit code page. oem.c wraps these with the
 * page it is currently holding; a platform with no oem.c calls them directly.
 * A code point with no OEM character becomes 0x7F, and so does a malformed
 * sequence, never an ASCII byte the input did not contain, because these
 * decode untrusted host filenames. An OEM byte with no code point becomes
 * U+FFFD. */
unsigned char unicode_from_codepoint(uint32_t cp, uint16_t page);
unsigned char unicode_from_utf8_next(const char **p, uint16_t page);
int unicode_to_utf8_char(unsigned char b, uint16_t page, char *dst);

#endif /* _CORE_STR_UNICODE_H_ */
