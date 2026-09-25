/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "osal/os.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

uint32_t os_random(void)
{
    struct timespec mono = {0}, real = {0};
    clock_gettime(CLOCK_MONOTONIC, &mono);
    clock_gettime(CLOCK_REALTIME, &real);
    uint64_t s = (uint64_t)mono.tv_nsec * 6364136223846793005ull +
                 (uint64_t)real.tv_sec * 1442695040888963407ull +
                 (uint64_t)real.tv_nsec + (uint64_t)(uintptr_t)&mono;
    return (uint32_t)(s ^ (s >> 32));
}

/* Apple's guidelines put an application's files in a folder of Application
 * Support named for its bundle identifier, and the config and the saves are
 * both in it. */
static char *app_support_dir(void)
{
    static const char tail[] =
        "/Library/Application Support/io.github.picocomputer.rp6502-emu";
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return NULL;
    size_t sz = strlen(home) + sizeof tail;
    char *dir = malloc(sz);
    if (dir)
        snprintf(dir, sz, "%s%s", home, tail);
    return dir;
}

char *os_config_dir(void)
{
    return app_support_dir();
}

char *os_save_dir(void)
{
    return app_support_dir();
}
