/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "osal/thread.h"
#include <process.h>
#include <stdlib.h>
#include <windows.h>

/* The value is the same on every SDK; only the newer ones declare it. */
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

struct os_thread
{
    HANDLE h;
    void (*fn)(void *);
    void *arg;
};

/* _beginthreadex rather than CreateThread, so the C runtime is set up for the
 * thread. */
static unsigned __stdcall os_thread_main(void *p)
{
    os_thread_t *t = p;
    t->fn(t->arg);
    return 0;
}

os_thread_t *os_thread_start(void (*fn)(void *), void *arg)
{
    os_thread_t *t = malloc(sizeof *t);
    if (!t)
        return NULL;
    t->fn = fn;
    t->arg = arg;
    t->h = (HANDLE)_beginthreadex(NULL, 0, os_thread_main, t, 0, NULL);
    if (!t->h)
    {
        free(t);
        return NULL;
    }
    return t;
}

void os_thread_join(os_thread_t *t)
{
    WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
    free(t);
}

void os_thread_yield(void)
{
    SwitchToThread();
}

/* A critical section is taken again by the thread holding it. */
struct os_lock
{
    CRITICAL_SECTION cs;
};

os_lock_t *os_lock_new(void)
{
    os_lock_t *l = malloc(sizeof *l);
    if (l)
        InitializeCriticalSection(&l->cs);
    return l;
}

void os_lock_free(os_lock_t *l)
{
    DeleteCriticalSection(&l->cs);
    free(l);
}

void os_lock(os_lock_t *l) { EnterCriticalSection(&l->cs); }

void os_unlock(os_lock_t *l) { LeaveCriticalSection(&l->cs); }

/* An auto-reset event is the kept signal. The timeout is a high-resolution
 * waitable timer, as os_sleep_ns's is, because a wait's own timeout rounds up
 * to the scheduler's tick, 15.6 ms unless something raised its rate. Windows
 * before 1803 refuses the flag, and the wait's own timeout serves there. One
 * thread waits on a wake, so the timer is the wake's. */
struct os_wake
{
    HANDLE event;
    HANDLE timer;
};

os_wake_t *os_wake_new(void)
{
    os_wake_t *w = malloc(sizeof *w);
    if (!w)
        return NULL;
    w->event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!w->event)
    {
        free(w);
        return NULL;
    }
    w->timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                      TIMER_ALL_ACCESS);
    return w;
}

void os_wake_free(os_wake_t *w)
{
    if (w->timer)
        CloseHandle(w->timer);
    CloseHandle(w->event);
    free(w);
}

void os_wake_signal(os_wake_t *w)
{
    SetEvent(w->event);
}

bool os_wake_wait(os_wake_t *w, uint64_t ns)
{
    if (!w->timer)
    {
        const DWORD ms = (DWORD)((ns + 999999ull) / 1000000ull);
        return WaitForSingleObject(w->event, ms) == WAIT_OBJECT_0;
    }
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)((ns + 99ull) / 100ull); /* relative, 100 ns units */
    SetWaitableTimer(w->timer, &due, 0, NULL, NULL, FALSE);
    const HANDLE both[2] = {w->event, w->timer};
    const DWORD got = WaitForMultipleObjects(2, both, FALSE, INFINITE);
    CancelWaitableTimer(w->timer);
    return got == WAIT_OBJECT_0;
}
