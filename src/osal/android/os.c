/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Android host-OS primitives that differ from the shared osal/posix/os.c:
 * entropy and the frame-pacer sleep (clock_nanosleep, absolute). Everything
 * else lives in osal/posix/os.c.
 *
 * /dev/urandom rather than getrandom(2), which bionic gates behind API 28 --
 * and the libretro core builds against whatever floor its frontend templates
 * pick. The file is there on every Android; what reads the result is a seed
 * for core/sys/random.c, not a key.
 */

#include "osal/os.h"
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

uint32_t os_random_seed(void)
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

void os_sleep_until_ns(uint64_t target)
{
    struct timespec until = {.tv_sec = (time_t)(target / 1000000000ull),
                             .tv_nsec = (long)(target % 1000000000ull)};
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, NULL);
}
