/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The one POSIX primitive this host answers itself: entropy. Everything else
 * a POSIX system has to say is in host/posix/host.c, and there is no
 * frame-pacer sleep here because the frontend paces the core — nothing in
 * this host ever waits.
 *
 * /dev/urandom rather than getrandom(2), which is Linux's and is gated
 * behind API 28 on Android. This one host spans Linux, macOS and Android,
 * and the file is there on all of them; what reads the result is the
 * reproducibility PRNG in core/sys/rand.c, not a key.
 */

#include "host.h"
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

uint64_t host_entropy_64(void)
{
    uint64_t s;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0)
    {
        ssize_t got = read(fd, &s, sizeof s);
        close(fd);
        if (got == (ssize_t)sizeof s && s)
            return s;
    }
    struct timespec mono = {0}, real = {0};
    clock_gettime(CLOCK_MONOTONIC, &mono);
    clock_gettime(CLOCK_REALTIME, &real);
    s = (uint64_t)mono.tv_nsec * 6364136223846793005ull +
        (uint64_t)real.tv_sec * 1442695040888963407ull +
        (uint64_t)real.tv_nsec + (uint64_t)(uintptr_t)&mono;
    return s ? s : 1;
}
