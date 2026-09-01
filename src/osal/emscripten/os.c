/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Emscripten host-OS primitives that differ from the shared osal/posix/os.c: entropy
 * (no getrandom) and the frame-pacer sleep (a no-op; requestAnimationFrame paces
 * the web loop). Everything else lives in osal/posix/os.c.
 */

#include "osal/os.h"
#include <time.h>

/* host_random_seed is not here: this machine links the sokol app's main.c,
 * which decides the run's seed for every desktop and the page alike. */
uint32_t os_random_seed(void)
{
    struct timespec mono = {0}, real = {0};
    clock_gettime(CLOCK_MONOTONIC, &mono);
    clock_gettime(CLOCK_REALTIME, &real);
    uint64_t s = (uint64_t)mono.tv_nsec * 6364136223846793005ull +
                 (uint64_t)real.tv_sec * 1442695040888963407ull +
                 (uint64_t)real.tv_nsec + (uint64_t)(uintptr_t)&mono;
    return (uint32_t)(s ^ (s >> 32));
}

void os_sleep_until_ns(uint64_t target)
{
    (void)target; /* requestAnimationFrame paces the web loop */
}
