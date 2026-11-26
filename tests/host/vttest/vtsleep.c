/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* vttest's pauses, made as short as they can be. vtrun preloads this into
 * vttest. A pause tells vtrun, which takes in everything vttest wrote
 * before it and answers it, sees that everything it has sent vttest is
 * there for vttest to read, then ends the pause. vttest pauses for two
 * reasons: to give a terminal time for a reply, which is in by then, and to
 * let a person look, which nobody does here.
 *
 * A pause ends when vtrun says, and only then. It reads nothing on the way:
 * what vttest has read, which vtrun counts in /proc, is only what it read
 * from its terminal. VTSLEEP_FD names the pipe a pause tells vtrun on, with
 * the process that pauses, and vtrun ends the pause with SIGUSR1. Without it
 * a pause is a pause. */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static int vs_tell = -1;
static sigset_t vs_end;
static int vs_ready;

/* SIGUSR1 is held back, so it waits for the pause that takes it. */
static void vs_init(void)
{
    if (vs_ready)
        return;
    vs_ready = 1;
    const char *fd = getenv("VTSLEEP_FD");
    if (!fd || sscanf(fd, "%d", &vs_tell) != 1)
    {
        vs_tell = -1;
        return;
    }
    sigemptyset(&vs_end);
    sigaddset(&vs_end, SIGUSR1);
    sigprocmask(SIG_BLOCK, &vs_end, NULL);
}

/* A pause, over when vtrun says. False when there is no vtrun to ask. */
static int vs_pause(void)
{
    vs_init();
    if (vs_tell < 0)
        return 0;
    int saved = errno;
    pid_t me = getpid();
    if (write(vs_tell, &me, sizeof me) == sizeof me)
        while (sigwaitinfo(&vs_end, NULL) < 0 && errno == EINTR)
            ;
    errno = saved;
    return 1;
}

int usleep(useconds_t us)
{
    if (vs_pause())
        return 0;
    int (*next)(useconds_t) = (int (*)(useconds_t))dlsym(RTLD_NEXT, "usleep");
    return next(us);
}

unsigned int sleep(unsigned int s)
{
    if (vs_pause())
        return 0;
    unsigned int (*next)(unsigned int) = (unsigned int (*)(unsigned int))dlsym(RTLD_NEXT, "sleep");
    return next(s);
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    if (vs_pause())
    {
        if (rem)
            rem->tv_sec = rem->tv_nsec = 0;
        return 0;
    }
    int (*next)(const struct timespec *, struct timespec *) =
        (int (*)(const struct timespec *, struct timespec *))dlsym(RTLD_NEXT, "nanosleep");
    return next(req, rem);
}
