
/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_STR_STR_H_
#define _CORE_STR_STR_H_

/*
 * String constants in flash and
 * miscellaneous string functions.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

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

// Format a byte count as a short human string ("119.1 GB", "1.44 MB", "512 KB").
// Media under 5 MB is shown in KB/MB; larger media in decimal MB/GB/TB.
void str_size(uint64_t bytes, char *out, size_t out_size);

// Non-localized string literals are in flash, or in RAM via XR().
#define X(name, value) \
    extern const char name[];
#define XR(name, value) X(name, value)
#include "core/def/str_sys.def"
#undef X
#undef XR

// Provide length of non-localized string literals.
#define X(name, value)                 \
    enum                               \
    {                                  \
        name##_LEN = sizeof(value) - 1 \
    };
#define XR(name, value) X(name, value)
#include "core/def/str_sys.def"
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
#include "core/def/str_en.def" // canonical key order; values ignored in this pass
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
const char *str_get_locale_verbose(void);

/* Stringize, for a row default that has to name the build's locale. */
#define STR_XSTR1(x) #x
#define STR_XSTR(x) STR_XSTR1(x)

/* Two letters and a terminator, with room to spare. */
#define STR_LOCALE_SIZE 8

/* This driver's setting; see core/sys/config.h. str_get_locale and
 * str_set_locale are generated from the row below. */
bool str_check_locale(const char *in, char *out);
void str_apply_locale(const char *name, bool changed);
int str_locale_response(char *buf, size_t buf_size, int state, unsigned width);

/* This driver's row in a machine's driver list; see core/sys/driver.h. */
#define STR_CONFIG_LOCALE CONFIG_STR(M, str, locale, STR_LOCALE_SIZE, STR_XSTR(RP6502_LOCALE), \
    str_check_locale, str_apply_locale, STR_LOC, str_locale_response, \
    STR_HELP_SET_LOC, str_locales_response)
#define STR_DRIVER DRIVER(str_init, nul_task, nul_task, nul_run, nul_stop, nul_break, \
    STR_CONFIG_LOCALE, nul_config)

#endif /* _CORE_STR_STR_H_ */
