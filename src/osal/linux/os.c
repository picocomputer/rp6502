/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "osal/os.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// The device rather than getrandom(), because <sys/random.h> arrived in glibc
// 2.25 and the libretro buildbot builds this core on Ubuntu Xenial, which has
// 2.23. Reading the device needs no version test and is what the Android layer
// beside this one already does.
uint32_t os_random(void)
{
    uint64_t s;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0)
    {
        ssize_t got = read(fd, &s, sizeof s);
        close(fd);
        if (got == (ssize_t)sizeof s && s)
            return (uint32_t)(s ^ (s >> 32));
    }
    struct timespec mono = {0}, real = {0};
    clock_gettime(CLOCK_MONOTONIC, &mono);
    clock_gettime(CLOCK_REALTIME, &real);
    s = (uint64_t)mono.tv_nsec * 6364136223846793005ull +
        (uint64_t)real.tv_sec * 1442695040888963407ull +
        (uint64_t)real.tv_nsec + (uint64_t)(uintptr_t)&mono;
    return (uint32_t)(s ^ (s >> 32));
}

/* app in an XDG base directory: $var when it is set and absolute, as the XDG
 * Base Directory spec requires, else fallback under $HOME. */
static char *xdg_dir(const char *var, const char *fallback, const char *app)
{
    const char *base = getenv(var);
    const char *mid = "";
    if (!base || base[0] != '/')
    {
        base = getenv("HOME");
        mid = fallback;
    }
    if (!base || !base[0])
        return NULL;
    size_t sz = strlen(base) + strlen(mid) + strlen(app) + 1;
    char *dir = malloc(sz);
    if (dir)
        snprintf(dir, sz, "%s%s%s", base, mid, app);
    return dir;
}

char *os_config_dir(void)
{
    return xdg_dir("XDG_CONFIG_HOME", "/.config", "/rp6502-emu");
}

char *os_save_dir(void)
{
    return xdg_dir("XDG_DATA_HOME", "/.local/share", "/rp6502");
}
