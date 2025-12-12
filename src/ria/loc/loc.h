/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_LOC_LOC_H_
#define _RIA_LOC_LOC_H_

// CONTRIBUTE: Duplicate one of the existing locale files
// then select your new RP6502_LOCALE in CMakeLists.txt.
// Optionally, implement a pluralizer in loc.c.

#define _LOC_STRINGIFY(x) #x
#define _LOC_CONCAT_(a, b) a##b
#define _LOC_CONCAT(a, b) _LOC_CONCAT_(a, b)
#define _LOC_MAKE_FILENAME(base) _LOC_STRINGIFY(base.inc)
#define LOC_LOCALE_FILE _LOC_MAKE_FILENAME(_LOC_CONCAT(loc_, RP6502_LOCALE))
#define LOC(suffix, name) \
    extern const char LOC_##suffix[];
#include LOC_LOCALE_FILE
#undef LOC

#endif /* _RIA_LOC_LOC_H_ */
