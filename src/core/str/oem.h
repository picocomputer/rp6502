/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_STR_OEM_H_
#define _CORE_STR_OEM_H_

/* The OEM driver manages IBM/DOS style code pages.
 * This affects RP6502-VGA, FatFs, and keyboards.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Guarded the way core/sys/com.h and the pico-sdk guard it, so whichever header
 * arrives first wins and the other skips. */
#ifndef __printflike
#ifdef __GNUC__
#define __printflike(a, b) __attribute__((__format__(__printf__, a, b)))
#else
#define __printflike(a, b)
#endif
#endif

/* Main events
 */

void oem_init(void);
void oem_stop(void);

// Code page without saving to config
void oem_set_code_page_run(uint16_t cp);
uint16_t oem_get_code_page_run(void);

// Configuration setting CP
bool oem_check_code_page(uint16_t *v);
void oem_apply_code_page(uint16_t cp, bool changed);
int oem_code_page_response(char *buf, size_t buf_size, int state, unsigned width);
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

/* snprintf for UTF-8 source text destined for the code page: format, then
 * convert. The conversion only ever contracts -- a multi-byte sequence
 * becomes one OEM byte -- so it runs in place and needs no scratch buffer.
 * Returns the converted length, as snprintf does. Converting in place is
 * what rules out snprintf's measure-with-a-null-buffer idiom: dst must be
 * a real buffer and dst_size at least one. */
__printflike(3, 4) int oem_snprintf(char *dst, size_t dst_size,
                                    const char *utf8_fmt, ...);
int oem_vsnprintf(char *dst, size_t dst_size, const char *utf8_fmt, va_list va);

/* This driver's row in a machine's driver list; see core/sys/driver.h. */
#define OEM_CONFIG_CODE_PAGE CONFIG_INT(S, oem, code_page, uint16_t, 0, \
    oem_check_code_page, oem_apply_code_page, STR_CP, oem_code_page_response, \
    STR_HELP_SET_CP, NULL)
#define OEM_DRIVER DRIVER(oem_init, nul_task, nul_task, nul_run, oem_stop, nul_break, \
    OEM_CONFIG_CODE_PAGE, nul_config)

#endif /* _CORE_STR_OEM_H_ */
