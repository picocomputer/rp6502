/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The two helpers tb_hostos.h declares, on a POSIX host. The build picks
 * this file or its Windows sibling; neither carries the other's spelling.
 */

#include "tb_hostos.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h> /* mkdtemp: stdlib.h on glibc, here on macOS */

bool host_make_tmpdir(char *buf, size_t sz)
{
    char tmpl[] = "/tmp/rp6502_test_XXXXXX";
    const char *d = mkdtemp(tmpl);
    if (!d || strlen(d) >= sz)
        return false;
    memcpy(buf, d, strlen(d) + 1);
    return true;
}

void host_setenv(const char *name, const char *value)
{
    setenv(name, value, 1);
}
