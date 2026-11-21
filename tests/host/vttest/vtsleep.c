/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* vttest's pauses, made as short as they can be. vtrun preloads this into
 * vttest. A pause tells vtrun, which takes in everything vttest wrote
 * before it and answers it, then lets the pause end. vttest pauses for two
 * reasons: to give a terminal time for a reply, which is in by then, and to
 * let a person look, which nobody does here. VTSLEEP_FDS names the pipe the
 * pause tells vtrun on and the one it waits on, "tell,wait"; without it a
 * pause is a pause. */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static int vs_tell = -1, vs_wait = -1;
static int vs_ready;

static void vs_init(void)
{
    if (vs_ready)
        return;
    vs_ready = 1;
    const char *fds = getenv("VTSLEEP_FDS");
    if (!fds || sscanf(fds, "%d,%d", &vs_tell, &vs_wait) != 2)
        vs_tell = vs_wait = -1;
}

/* A pause of ms milliseconds at most: over when vtrun says. False when
 * there is no vtrun to ask. */
static int vs_pause(long ms)
{
    vs_init();
    if (vs_tell < 0)
        return 0;
    int saved = errno;
    char c = 's';
    if (write(vs_tell, &c, 1) == 1)
    {
        struct pollfd p = {.fd = vs_wait, .events = POLLIN};
        if (poll(&p, 1, (int)(ms > 0x7FFFFFFFL ? 0x7FFFFFFFL : ms)) > 0)
            (void)!read(vs_wait, &c, 1);
    }
    errno = saved;
    return 1;
}

int usleep(useconds_t us)
{
    if (vs_pause((long)(us / 1000)))
        return 0;
    int (*next)(useconds_t) = (int (*)(useconds_t))dlsym(RTLD_NEXT, "usleep");
    return next(us);
}

unsigned int sleep(unsigned int s)
{
    if (vs_pause((long)s * 1000))
        return 0;
    unsigned int (*next)(unsigned int) = (unsigned int (*)(unsigned int))dlsym(RTLD_NEXT, "sleep");
    return next(s);
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    if (vs_pause(req->tv_sec * 1000L + req->tv_nsec / 1000000L))
    {
        if (rem)
            rem->tv_sec = rem->tv_nsec = 0;
        return 0;
    }
    int (*next)(const struct timespec *, struct timespec *) =
        (int (*)(const struct timespec *, struct timespec *))dlsym(RTLD_NEXT, "nanosleep");
    return next(req, rem);
}
