
/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _STR_H_
#define _STR_H_

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * Miscellaneous string functions. TODO: (pick one)
 * This could evolve to cleaner tokenizing and parsing.
 * Or devolve into a dumping ground for wayward functions.
 */

// Test for 0-9 a-f A-F
bool char_is_hex(char ch);

// Change chars 0-9 a-f A-F to a binary int, -1 on fail
int char_to_int(char ch);

// Case insensitive string compare with length limit
int strnicmp(const char *string1, const char *string2, int n);

// A single argument in hex or decimal. e.g. 0x0, $0, 0
bool parse_uint32(const char **args, size_t *len, uint32_t *result);

// A ROM name converted to upper case.
// Only A-Z allowed in first character, A-Z0-9 for remainder.
// Return argument name must hold LFS_NAME_MAX+1.
bool parse_rom_name(const char **args, size_t *len, char *name);

// Ensure there are no more arguments.
bool parse_end(const char *args, size_t len);

#endif /* _STR_H_ */
