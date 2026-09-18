/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tb_hostos.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

const char *host_drive(void)
{
    return "FS:";
}
