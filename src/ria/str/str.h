
/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_STR_STR_H_
#define _RIA_STR_STR_H_

/*
 * String constants in flash and
 * miscellaneous string functions.
 */

#include <fatfs/ff.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/cdefs.h>

// True if c is a path separator. FatFs accepts both '/' and '\'.
#define str_is_sep(c) ((c) == '/' || (c) == '\\')

// Converts a FatFS path to a fully-qualified absolute path,
// resolving the CWD for relative paths.
// Returns a pointer to static storage valid until the next str_* call.
// Returns NULL if the path exceeds 255 characters or CWD lookup fails.
const char *str_abs_path(const char *path);

// Look up the on-disk filename for path (case-insensitive). On success
// fno->fname holds the name as stored on disk. f_stat returns the input
// case for LFN files; this iterates the parent via f_readdir to recover
// the real case.
FRESULT str_lookup_basename(const char *path, FILINFO *fno);

// Replace path's basename in place with the case stored on disk.
// Returns false only if the corrected path wouldn't fit in path_size
// (caller should treat as fatal). Returns true on success or when the
// lookup gracefully fails (path left unchanged).
bool str_correct_basename(char *path, size_t path_size);

// Change chars 0-9 a-f A-F to a binary int, no error checking.
int str_xdigit_to_int(char ch);

// Parse a string, optionally quoted with escape sequences.
// Returns a pointer to static storage valid until the next str_* call.
// Returns NULL if no token is present, a null byte is produced, or the
// output would exceed 255 characters.
const char *str_parse_string(const char **args);

// A single argument in hex or decimal. e.g. 0x0, $0, 0
bool str_parse_uint8(const char **args, uint8_t *result);

// A single argument in hex or decimal. e.g. 0x0, $0, 0
bool str_parse_uint16(const char **args, uint16_t *result);

// A single argument in hex or decimal. e.g. 0x0, $0, 0
bool str_parse_uint32(const char **args, uint32_t *result);

// Ensure there are no more arguments (only spaces to the null terminator).
bool str_parse_end(const char *args);

// Decode one UTF-8 codepoint at *p, advance *p by 1-4 bytes, and return
// the OEM byte for the active code page. Returns 0x7F when the codepoint
// has no mapping or the UTF-8 is malformed (the lead byte is consumed).
// Returns 0 at end of string without advancing.
unsigned char str_utf8_to_oem(const char **p);

// printf where utf8_fmt and any %s args are treated as UTF-8.
// Output bytes are UTF-8 -> OEM-converted (active code page) via putchar.
__printflike(1, 2) int printf_utf8(const char *utf8_fmt, ...);
int vprintf_utf8(const char *utf8_fmt, va_list va);

// snprintf with the same UTF-8 -> OEM treatment; result is OEM bytes in dst.
__printflike(3, 4) int snprintf_utf8(char *dst, size_t dst_size,
                                     const char *utf8_fmt, ...);
int vsnprintf_utf8(char *dst, size_t dst_size,
                   const char *utf8_fmt, va_list va);

// String literals are in flash, or in RAM via XR().
#define X(name, value) \
    extern const char name[];
#define XR(name, value) X(name, value)
#include "str.def"
#include "str_locale.def"
#undef X
#undef XR

// Provide length of non-localized string literals.
#define X(name, value)                 \
    enum                               \
    {                                  \
        name##_LEN = sizeof(value) - 1 \
    };
#define XR(name, value) X(name, value)
#include "str.def"
#undef X
#undef XR

#endif /* _RIA_STR_STR_H_ */
