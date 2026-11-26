/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "osal/thread.h"
#include "osal/os.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <time.h>

struct os_thread
{
    pthread_t id;
    void (*fn)(void *);
    void *arg;
};

static void *os_thread_main(void *p)
{
    os_thread_t *t = p;
    t->fn(t->arg);
    return NULL;
}

os_thread_t *os_thread_start(void (*fn)(void *), void *arg)
{
    os_thread_t *t = malloc(sizeof *t);
    if (!t)
        return NULL;
    t->fn = fn;
    t->arg = arg;
    if (pthread_create(&t->id, NULL, os_thread_main, t))
    {
        free(t);
        return NULL;
    }
    return t;
}

void os_thread_join(os_thread_t *t)
{
    pthread_join(t->id, NULL);
    free(t);
}

void os_thread_yield(void)
{
    sched_yield();
}

struct os_lock
{
    pthread_mutex_t m;
};

os_lock_t *os_lock_new(void)
{
    os_lock_t *l = malloc(sizeof *l);
    if (!l)
        return NULL;
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    const int err = pthread_mutex_init(&l->m, &a);
    pthread_mutexattr_destroy(&a);
    if (err)
    {
        free(l);
        return NULL;
    }
    return l;
}

void os_lock_free(os_lock_t *l)
{
    pthread_mutex_destroy(&l->m);
    free(l);
}

void os_lock(os_lock_t *l) { pthread_mutex_lock(&l->m); }

void os_unlock(os_lock_t *l) { pthread_mutex_unlock(&l->m); }

/* The flag is the kept signal, under the mutex the condition waits with. */
struct os_wake
{
    pthread_mutex_t m;
    pthread_cond_t c;
    bool signalled;
};

os_wake_t *os_wake_new(void)
{
    os_wake_t *w = malloc(sizeof *w);
    if (!w)
        return NULL;
    pthread_condattr_t a;
    pthread_condattr_init(&a);
#ifndef __APPLE__
    /* The timeout is on the monotonic clock, which a change to the date does
     * not move. macOS has no such attribute, and waits relative instead. */
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
#endif
    const bool ok = !pthread_mutex_init(&w->m, NULL);
    const bool cok = ok && !pthread_cond_init(&w->c, &a);
    pthread_condattr_destroy(&a);
    if (!cok)
    {
        if (ok)
            pthread_mutex_destroy(&w->m);
        free(w);
        return NULL;
    }
    w->signalled = false;
    return w;
}

void os_wake_free(os_wake_t *w)
{
    pthread_cond_destroy(&w->c);
    pthread_mutex_destroy(&w->m);
    free(w);
}

void os_wake_signal(os_wake_t *w)
{
    pthread_mutex_lock(&w->m);
    w->signalled = true;
    pthread_cond_signal(&w->c);
    pthread_mutex_unlock(&w->m);
}

bool os_wake_wait(os_wake_t *w, uint64_t ns)
{
    pthread_mutex_lock(&w->m);
#ifdef __APPLE__
    /* A wait that wakes for nothing waits again for what is left. */
    const uint64_t until = os_mono_ns() + ns;
    for (uint64_t now = os_mono_ns(); !w->signalled && now < until;
         now = os_mono_ns())
    {
        const uint64_t left = until - now;
        struct timespec rel = {(time_t)(left / 1000000000ull), (long)(left % 1000000000ull)};
        if (pthread_cond_timedwait_relative_np(&w->c, &w->m, &rel) == ETIMEDOUT)
            break;
    }
#else
    struct timespec until;
    clock_gettime(CLOCK_MONOTONIC, &until);
    until.tv_sec += (time_t)(ns / 1000000000ull);
    until.tv_nsec += (long)(ns % 1000000000ull);
    if (until.tv_nsec >= 1000000000L)
    {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    while (!w->signalled && pthread_cond_timedwait(&w->c, &w->m, &until) != ETIMEDOUT)
        ;
#endif
    const bool signalled = w->signalled;
    w->signalled = false;
    pthread_mutex_unlock(&w->m);
    return signalled;
}
