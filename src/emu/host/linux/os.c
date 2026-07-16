/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Linux host-OS primitives that differ from the shared posix/os.c: entropy
 * (getrandom) and the frame-pacer sleep (clock_nanosleep, absolute). Everything
 * else a POSIX host needs lives in posix/os.c.
 */

#include "emu/plat.h"
#include <sys/random.h>
#include <time.h>

uint64_t os_entropy_64(void)
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

void os_sleep_until_ns(uint64_t target)
{
    struct timespec until = {.tv_sec = (time_t)(target / 1000000000ull),
                             .tv_nsec = (long)(target % 1000000000ull)};
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, NULL);
}
