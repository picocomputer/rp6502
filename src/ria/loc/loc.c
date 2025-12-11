/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "loc/loc.h"
#include <string.h>
#include <ctype.h>

#if defined(DEBUG_RIA_LOC) || defined(DEBUG_RIA_LOC_LOC)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// Use the RP6502_LOCALE defined in CMakeLists.txt
#define LOC_LOCALE_(a, b) a##b
#define LOC_LOCALE LOC_LOCALE_(LOC_, RP6502_LOCALE)
