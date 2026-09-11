/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_STR_OEM_H_
#define _CORE_STR_OEM_H_

/* The OEM driver manages IBM/DOS style code pages, which the RP6502-VGA
 * display, FatFs, and keyboards all render or read text in. */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include "core/sys/sst.h"
#include <stdbool.h>

#ifndef __printflike
#ifdef __GNUC__
#define __printflike(a, b) __attribute__((__format__(__printf__, a, b)))
#else
#define __printflike(a, b)
#endif
#endif

void oem_init(void);
void oem_stop(void);

// The code page in force, set without saving it to config.
void oem_set_code_page_run(uint16_t cp);
uint16_t oem_get_code_page_run(void);

bool oem_check_code_page(uint16_t *v);
void oem_apply_code_page(uint16_t cp, bool changed);
int oem_code_page_response(char *buf, size_t buf_size, int state, unsigned width);
bool oem_is_auto(void);

void oem_locale_changed(uint16_t cp);

/* OEM <-> Unicode conversion in the code page in force. A character the
 * conversion cannot spell becomes 0x7F on the OEM side and U+FFFD on the
 * Unicode side. */

unsigned char oem_from_codepoint(uint32_t cp);

// One UTF-8 sequence -> one OEM byte; advances *p; returns 0 at the NUL
unsigned char oem_from_utf8_next(const char **p);

/* State a conversion carries between calls, so a CRLF divided between two of
 * them still ends one line. Zero it to start one. */
typedef struct
{
    bool after_cr;
} oem_run_t;

/* A run of UTF-8 becomes OEM bytes. A character the code page cannot spell
 * becomes '?' rather than the 0x7F the decoder returns, because the line
 * editor in core/str/rln.c reads 0x7F as a backspace. CR, LF, and CRLF each
 * become the single carriage return that ends a line, and every other control
 * byte passes through.
 *
 * The conversion stops on a full dst, and on a sequence the buffer does not
 * hold whole unless end says none is coming. *taken is what was read, so the
 * rest can be carried to the next call, and the return is the count
 * written. */
size_t oem_from_utf8_run(oem_run_t *run, const char *utf8, size_t len, bool end,
                         char *dst, size_t dstsz, size_t *taken);

// One OEM byte -> UTF-8 in dst (at most 3 bytes, no NUL); returns the count
int oem_to_utf8_char(unsigned char b, char *dst);

// Whole strings; snprintf-style return of the untruncated length
size_t oem_to_utf8(const char *s, char *dst, size_t dstsz);
size_t oem_from_utf8(const char *u8, char *dst, size_t dstsz);

/* Whether every character of a string has a representation, so converting it
 * gives back the same string rather than one with substitutions in it. A
 * substituted character in a filename is a different name, and opens a
 * different file or none. */
bool oem_maps_utf8(const char *u8);  // host UTF-8 -> the code page
bool oem_maps_wide(const uint16_t *w); // host UTF-16 -> the code page
bool oem_maps_oem(const char *s);      // the code page -> Unicode

// UTF-16 strings; returns units/bytes written
int oem_to_wide(const char *s, uint16_t *w, int wcount);
size_t oem_from_wide(const uint16_t *w, char *dst, size_t dstsz);
size_t oem_from_wide_n(const uint16_t *w, size_t wlen, char *dst, size_t dstsz);

/* snprintf for UTF-8 source text destined for the code page: format, then
 * convert. The conversion only ever contracts, because a multi-byte sequence
 * becomes one OEM byte, so it runs in place and needs no scratch buffer.
 * Returns the converted length of what was written, not snprintf's
 * untruncated length, so it cannot report truncation. Converting in place
 * rules out snprintf's measure-with-a-null-buffer idiom: dst must be a real
 * buffer and dst_size at least one. */
__printflike(3, 4) int oem_snprintf(char *dst, size_t dst_size,
                                    const char *utf8_fmt, ...);
int oem_vsnprintf(char *dst, size_t dst_size, const char *utf8_fmt, va_list va);

/* This driver's setting; see core/sys/config.h. */
#define OEM_CONFIG_CODE_PAGE CONFIG_INT(S, oem, code_page, uint16_t, 0, \
    oem_check_code_page, oem_apply_code_page, STR_CP, oem_code_page_response, \
    STR_HELP_SET_CP, NULL)
/* Only the code page in force is saved. The glyphs follow from that one
 * number, so the load asks for the page again and puts them back. */
#define OEM_SST_SIZE 2
void oem_sst_save(sst_cursor_t *c, unsigned flags);
bool oem_sst_load(sst_cursor_t *c, unsigned flags);

#define OEM_DRIVER DRIVER(oem_init, nul_task, nul_task, nul_run, oem_stop, nul_break, \
    OEM_CONFIG_CODE_PAGE, nul_config, SST(OEM_, 1, OEM_SST_SIZE, oem_sst_save, oem_sst_load))

#endif /* _CORE_STR_OEM_H_ */
