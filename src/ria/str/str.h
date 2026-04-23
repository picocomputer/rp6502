
/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_STR_STR_H_
#define _RIA_STR_STR_H_

// CONTRIBUTE: Duplicate one of the existing locale files then select your
// new RP6502_LOCALE in CMakeLists.txt. Localization may not be practical
// because only 7-bit ASCII is allowed. Or undecorated characters might
// feel more authentic. I don't know but it was easy to add as part of
// consolidating strings in flash.

/*
 * String constants in flash and
 * miscellaneous string functions.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Converts a FatFS path to a fully-qualified absolute path,
// resolving the CWD for relative paths.
// Returns a pointer to static storage valid until the next str_* call.
// Returns NULL if the path exceeds 255 characters or CWD lookup fails.
const char *str_abs_path(const char *path);

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

// String literals are in flash, or in RAM via XR().
#define X(name, value) \
    extern const char name[];
#define XR(name, value) X(name, value)
#include "str.inc"
#include RP6502_LOCALE
#undef X
#undef XR

// Provide length of non-localized string literals.
#define X(name, value)                 \
    enum                               \
    {                                  \
        name##_LEN = sizeof(value) - 1 \
    };
#define XR(name, value) X(name, value)
#include "str.inc"
#undef X
#undef XR

#endif /* _RIA_STR_STR_H_ */
