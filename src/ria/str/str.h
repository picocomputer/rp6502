
/*
 * Copyright (c) 2025 Rumbledethumps
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
 * Used by the monitor for parsing input.
 * Also used to parse config files.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Change chars 0-9 a-f A-F to a binary int, no error checking
int str_xdigit_to_int(char ch);

// Parse everything else as a string, truncating trailing spaces.
bool str_parse_string(const char **args, size_t *len, char *dest, size_t size);

// A single argument in hex or decimal. e.g. 0x0, $0, 0
bool str_parse_uint8(const char **args, size_t *len, uint8_t *result);

// A single argument in hex or decimal. e.g. 0x0, $0, 0
bool str_parse_uint16(const char **args, size_t *len, uint16_t *result);

// A single argument in hex or decimal. e.g. 0x0, $0, 0
bool str_parse_uint32(const char **args, size_t *len, uint32_t *result);

// A ROM name converted to upper case.
// Only A-Z allowed in first character, A-Z0-9 for remainder.
// Return argument name must hold LFS_NAME_MAX+1.
bool str_parse_rom_name(const char **args, size_t *len, char *name);

// Ensure there are no more arguments.
bool str_parse_end(const char *args, size_t len);

// Part 1 of putting string literals into flash.
#define _STR_STRINGIFY(x) #x
#define _STR_CONCAT_(a, b) a##b
#define _STR_CONCAT(a, b) _STR_CONCAT_(a, b)
#define _STR_MAKE_FILENAME(base) _STR_STRINGIFY(base.inc)
#define STR_LOCALE_FILE _STR_MAKE_FILENAME(_STR_CONCAT(str_, RP6502_LOCALE))
#define X(name, value) \
    extern const char name[];
#include STR_LOCALE_FILE
#include "str.inc"
#undef X

#endif /* _RIA_STR_STR_H_ */
