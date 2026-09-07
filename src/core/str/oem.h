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
#include "core/sys/sst.h"
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

/* A conversion in progress, because a clipboard is handed over a piece at a
 * time. Zeroed to start one. */
typedef struct
{
    /* The last byte out was a return, so a line feed opening the next call
     * is the other half of one this call did not hold. */
    bool after_cr;
} oem_run_t;

/* A run of host text -> OEM, which is what a clipboard holds: characters in
 * the host's encoding, ending lines the host's way. '?' stands in for what
 * the code page cannot spell, because the decoder's own stand-in is DEL and
 * a line editor reads that as a backspace. A newline in any of its three
 * spellings becomes the one return a line editor ends a line on, and every
 * other control byte passes through.
 *
 * Not for the console's wire, which carries whatever the far end sent and is
 * not the host's text to correct.
 *
 * Stops on a full dst, and on a sequence the buffer does not hold whole
 * unless end says none is coming; *taken is what was read, so the rest can
 * be carried to the next call. Returns the count written. */
size_t oem_from_utf8_run(oem_run_t *run, const char *utf8, size_t len, bool end,
                         char *dst, size_t dstsz, size_t *taken);

// One OEM byte -> UTF-8 in dst (at most 3 bytes, no NUL); returns the count
int oem_to_utf8_char(unsigned char b, char *dst);

// Whole strings; snprintf-style return of the untruncated length
size_t oem_to_utf8(const char *s, char *dst, size_t dstsz);
size_t oem_from_utf8(const char *u8, char *dst, size_t dstsz);

/* Whether every character of a string has a representation, so converting it
 * gives the same string rather than one with substitutions in it. A filename
 * is what asks: a substituted character is a different name, and it opens a
 * different file or none. Text a person reads does not ask, and substitutes. */
bool oem_maps_utf8(const char *u8);  // host UTF-8 -> the code page
bool oem_maps_wide(const uint16_t *w); // host UTF-16 -> the code page
bool oem_maps_oem(const char *s);      // the code page -> Unicode

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
/* The page in force, and nothing derived from it. The glyph tables and the
 * conversion tables are a function of this one number, so the load asks for
 * the page again and lets them be rebuilt. The setting behind it is the
 * host's and stays behind, as does the locale's own answer. */
#define OEM_SST_SIZE 2
void oem_sst_save(sst_cursor_t *c, unsigned flags);
bool oem_sst_load(sst_cursor_t *c, unsigned flags);

#define OEM_DRIVER DRIVER(oem_init, nul_task, nul_task, nul_run, oem_stop, nul_break, \
    OEM_CONFIG_CODE_PAGE, nul_config, SST(OEM_, 1, OEM_SST_SIZE, oem_sst_save, oem_sst_load))

#endif /* _CORE_STR_OEM_H_ */
