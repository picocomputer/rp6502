/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "osal/os.h"
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

uint32_t os_random(void)
{
    uint64_t s;
    /* Not getrandom(2): bionic gates it behind API 28. */
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
