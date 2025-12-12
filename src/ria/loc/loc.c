/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "loc/loc.h"
#include <string.h>
#include <ctype.h>
#include <pico.h>

#if defined(DEBUG_RIA_LOC) || defined(DEBUG_RIA_LOC_LOC)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define LOC(suffix, name) \
    const char __in_flash("loc_strings") LOC_##suffix[] = name;
#include LOC_LOCALE_FILE
#undef LOC
