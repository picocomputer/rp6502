/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * macOS host-OS primitives that differ from the shared posix/os.c: entropy (no
 * getrandom) and the frame-pacer sleep (relative nanosleep, since macOS lacks
 * clock_nanosleep/TIMER_ABSTIME). Everything else lives in posix/os.c.
 */

#include "emu/plat.h"
#include <errno.h>
#include <time.h>

uint64_t os_entropy_64(void)
{
    struct timespec mono = {0}, real = {0};
    clock_gettime(CLOCK_MONOTONIC, &mono);
    clock_gettime(CLOCK_REALTIME, &real);
    uint64_t s = (uint64_t)mono.tv_nsec * 6364136223846793005ull +
                 (uint64_t)real.tv_sec * 1442695040888963407ull +
                 (uint64_t)real.tv_nsec + (uint64_t)(uintptr_t)&mono;
    return s ? s : 1;
}

void os_sleep_until_ns(uint64_t target)
{
    uint64_t now = os_mono_ns();
    if (target > now)
    {
        uint64_t delta = target - now;
        struct timespec req = {.tv_sec = (time_t)(delta / 1000000000ull),
                               .tv_nsec = (long)(delta % 1000000000ull)};
        while (nanosleep(&req, &req) != 0 && errno == EINTR)
            ;
    }
}
