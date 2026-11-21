/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/com/com.h"

/* The RIA answers the program's queries. Its terminal follows this one, so
 * an answer here is dropped. */
void com_reply(const char *s, size_t n)
{
    (void)s;
    (void)n;
}
