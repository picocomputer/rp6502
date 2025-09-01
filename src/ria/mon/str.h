
/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_MON_STR_H_
#define _RIA_MON_STR_H_

/*
 * Miscellaneous string functions.
 * Used by the monitor for parsing input.
 * Also used to parse config files.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Test for 0-9 a-f A-F
bool str_char_is_hex(char ch);

// Change chars 0-9 a-f A-F to a binary int, -1 on fail
int str_char_to_int(char ch);

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

#endif /* _RIA_MON_STR_H_ */
