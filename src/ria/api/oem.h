/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_API_OEM_H_
#define _RIA_API_OEM_H_

/* The OEM driver manages IBM/DOS style code pages.
 * This affects RP6502-VGA, FatFs, and keyboards.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void oem_init(void);
void oem_stop(void);

// Code page without saving to config
void oem_set_code_page_run(uint16_t cp);
uint16_t oem_get_code_page_run(void);

// Configuration setting CP
void oem_load_code_page(const char *str);
bool oem_set_code_page(uint16_t cp);
uint16_t oem_get_code_page(void);
bool oem_is_auto(void);

// Set the locale's default
void oem_locale_changed(uint16_t cp);

/* OEM <-> Unicode conversion in the running code page.
 * Unmappable input becomes 0x7F (OEM side) or U+FFFD (Unicode side).
 */

// One Unicode codepoint -> one OEM byte
unsigned char oem_from_codepoint(uint32_t cp);

// One UTF-8 sequence -> one OEM byte; advances *p; returns 0 at the NUL
unsigned char oem_from_utf8_next(const char **p);

// One OEM byte -> UTF-8 in dst (at most 3 bytes, no NUL); returns the count
int oem_to_utf8_char(unsigned char b, char *dst);

// Whole strings; snprintf-style return of the untruncated length
size_t oem_to_utf8(const char *s, char *dst, size_t dstsz);
size_t oem_from_utf8(const char *u8, char *dst, size_t dstsz);

// UTF-16 strings; returns units/bytes written
int oem_to_wide(const char *s, uint16_t *w, int wcount);
size_t oem_from_wide(const uint16_t *w, char *dst, size_t dstsz);
size_t oem_from_wide_n(const uint16_t *w, size_t wlen, char *dst, size_t dstsz);

#endif /* _RIA_API_OEM_H_ */
