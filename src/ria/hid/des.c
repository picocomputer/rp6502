/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "pico.h"
#include "hid/des.h"

#if defined(DEBUG_HID_USB) || defined(DEBUG_RIA_HID_DES)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif
