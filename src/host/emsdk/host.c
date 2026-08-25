/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Emscripten host-OS primitives that differ from the shared host/posix/host.c: entropy
 * (no getrandom) and the frame-pacer sleep (a no-op; requestAnimationFrame paces
 * the web loop). Everything else lives in host/posix/host.c.
 */

#include "host.h"
#include "host/sokol/window.h" /* host_sleep_until_ns */
#include <time.h>

uint64_t host_entropy_64(void)
{
    struct timespec mono = {0}, real = {0};
    clock_gettime(CLOCK_MONOTONIC, &mono);
    clock_gettime(CLOCK_REALTIME, &real);
    uint64_t s = (uint64_t)mono.tv_nsec * 6364136223846793005ull +
                 (uint64_t)real.tv_sec * 1442695040888963407ull +
                 (uint64_t)real.tv_nsec + (uint64_t)(uintptr_t)&mono;
    return s ? s : 1;
}

void host_sleep_until_ns(uint64_t target)
{
    (void)target; /* requestAnimationFrame paces the web loop */
}
