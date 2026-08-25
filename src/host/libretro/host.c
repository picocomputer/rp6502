/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The one host-OS primitive that differs from the shared core/posix/host.c:
 * entropy. There is no frame-pacer sleep here — the frontend paces the core,
 * so nothing in this host ever waits.
 */

#include "host.h"
#include "core/emu/app/rand.h" /* host_entropy_64 */
#include <sys/random.h>
#include <time.h>

uint64_t host_entropy_64(void)
{
    uint64_t s;
    if (getrandom(&s, sizeof s, 0) == (ssize_t)sizeof s && s)
        return s;
    struct timespec mono = {0}, real = {0};
    clock_gettime(CLOCK_MONOTONIC, &mono);
    clock_gettime(CLOCK_REALTIME, &real);
    s = (uint64_t)mono.tv_nsec * 6364136223846793005ull +
        (uint64_t)real.tv_sec * 1442695040888963407ull +
        (uint64_t)real.tv_nsec + (uint64_t)(uintptr_t)&mono;
    return s ? s : 1;
}
