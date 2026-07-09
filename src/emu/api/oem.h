/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_API_OEM_H_
#define _EMU_API_OEM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

void oem_reset(void);                /* reset to the default code page (437) */
uint16_t oem_get_code_page(void);    /* the active OEM code page */
bool oem_set_code_page(uint16_t cp); /* set the active code page; false if unsupported */

/* The firmware oem.h "run" names the vendored atr.c uses. No config-vs-running
 * split in the emulator, so these are the effective page (best-effort set). */
uint16_t oem_get_code_page_run(void);
void oem_set_code_page_run(uint16_t cp);

/* One UTF-8 sequence -> one OEM byte in the active code page (firmware
 * str_utf8_to_oem): ASCII passes through, a valid codepoint maps via ff_uni2oem,
 * anything unmappable or malformed becomes 0x7F. Advances *p past the sequence;
 * returns 0 only at the terminating NUL. */
unsigned char oem_utf8_to_oem(const char **p);

/* Inverse of oem_utf8_to_oem: one OEM byte in the active code page -> its UTF-8
 * bytes written to dst (at most 3, no NUL); returns the count. Unmappable high
 * bytes become U+FFFD, so the result is always valid UTF-8. */
int oem_to_utf8(unsigned char b, char *dst);

/* OEM (active code page) <-> UTF-16, for platforms whose native filesystem API is
 * wide (Win32 …W). oem_to_wide writes up to wcount-1 code units + a 0 terminator and
 * returns the unit count; oem_from_wide is the inverse (unmappable -> 0x7F). */
int oem_to_wide(const char *s, uint16_t *w, int wcount);
size_t oem_from_wide(const uint16_t *w, char *dst, size_t dstsz);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_API_OEM_H_ */
