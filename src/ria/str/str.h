
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

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* The Pico/BSD toolchains supply __printflike via <sys/cdefs.h>; provide it
 * ourselves so this header also compiles on the emulator hosts (glibc has no
 * __printflike, Emscripten no <sys/cdefs.h> at all). */
#ifndef __printflike
#ifdef __GNUC__
#define __printflike(fmtarg, firstvararg) \
    __attribute__((__format__(__printf__, fmtarg, firstvararg)))
#else
#define __printflike(fmtarg, firstvararg)
#endif
#endif

// True if c is a path separator. FatFs accepts both '/' and '\'.
#define str_is_sep(c) ((c) == '/' || (c) == '\\')

// Converts a FatFS path to a fully-qualified absolute path,
// resolving the CWD for relative paths.
// Returns a pointer to static storage valid until the next str_* call.
// Returns NULL if the path exceeds 255 characters or CWD lookup fails.
const char *str_abs_path(const char *path);

// Look up the on-disk filename for path (case-insensitive). On success
// writes the on-disk basename (NUL-terminated) to out and returns true.
// f_stat returns the input case for LFN files; this iterates the parent
// via f_readdir to recover the real case. out_size must be at least
// FF_LFN_BUF + 1 bytes (256) to fit any FatFs LFN.
bool str_lookup_basename(const char *path, char *out, size_t out_size);

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

// Case-insensitive equality of two OEM strings in the active code page (uses
// FatFs code-page tables and up-case folding; strcasecmp folds only ASCII).
bool str_oem_eq(const char *a, const char *b);

// printf where utf8_fmt and any %s args are treated as UTF-8.
// Output bytes are UTF-8 -> OEM-converted (active code page) via putchar.
__printflike(1, 2) int printf_utf8(const char *utf8_fmt, ...);
int vprintf_utf8(const char *utf8_fmt, va_list va);

// snprintf with the same UTF-8 -> OEM treatment; result is OEM bytes in dst.
__printflike(3, 4) int snprintf_utf8(char *dst, size_t dst_size,
                                     const char *utf8_fmt, ...);
int vsnprintf_utf8(char *dst, size_t dst_size,
                   const char *utf8_fmt, va_list va);

// Format a byte count as a short human string ("119.1 GB", "1.44 MB", "512 KB").
// Media under 5 MB is shown in KB/MB; larger media in decimal MB/GB/TB.
void str_size(uint64_t bytes, char *out, size_t out_size);

// Non-localized string literals are in flash, or in RAM via XR().
#define X(name, value) \
    extern const char name[];
#define XR(name, value) X(name, value)
#include "def/str_sys.def"
#undef X
#undef XR

// Provide length of non-localized string literals.
#define X(name, value)                 \
    enum                               \
    {                                  \
        name##_LEN = sizeof(value) - 1 \
    };
#define XR(name, value) X(name, value)
#include "def/str_sys.def"
#undef X
#undef XR

// Localized strings. Each name is an id (an index), not a pointer; S(id)
// returns the active locale's string. The locale is selected by name with
// the str_*_locale API below. The compiled-in locales are listed in def/str.def.
enum str_loc_id
{
#define XBEGIN(code, verbose, cp)
#define XEND()
#define X(name, value) name,
#define XR(name, value) X(name, value)
#include "def/str_en.def" // canonical key order; values ignored in this pass
#undef XBEGIN
#undef XEND
#undef X
#undef XR
    STR_LOC_COUNT
};

// Active-locale string for a localized id.
const char *S(int id);

// Initialize the string module (establishes the build-default locale).
void str_init(void);

// Locale (UI language) selection.
int str_locales_response(char *buf, size_t buf_size, int state, unsigned width);
void str_load_locale(const char *name);
bool str_set_locale(const char *name);
const char *str_get_locale(void);
const char *str_get_locale_verbose(void);

#endif /* _RIA_STR_STR_H_ */
