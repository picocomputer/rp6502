/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _OSAL_THREAD_H_
#define _OSAL_THREAD_H_

/* Threads, a lock and a wake for the desktop emulator, which runs its machine
 * on a thread of its own (host/sokol/app/run.c). Only the desktop builds have
 * these: the web player runs on the browser's one thread. */

#include <stdbool.h>
#include <stdint.h>

typedef struct os_thread os_thread_t;
typedef struct os_lock os_lock_t;
typedef struct os_wake os_wake_t;

/* A thread running fn(arg), or NULL when the OS gives none. */
os_thread_t *os_thread_start(void (*fn)(void *), void *arg);

/* Waits for the thread to return, and frees it. */
void os_thread_join(os_thread_t *t);

/* Gives the rest of this thread's turn to another that is ready. */
void os_thread_yield(void);

/* A lock the thread holding it may take again, as long as it lets go as many
 * times. NULL when the OS gives none. */
os_lock_t *os_lock_new(void);
void os_lock_free(os_lock_t *l);
void os_lock(os_lock_t *l);
void os_unlock(os_lock_t *l);

/* A wake that one thread waits on and any thread signals. A signal while no
 * one waits is kept for the next wait, and a wait takes it, so a signal is
 * never lost and wakes one wait. NULL when the OS gives none. */
os_wake_t *os_wake_new(void);
void os_wake_free(os_wake_t *w);
void os_wake_signal(os_wake_t *w);

/* Waits for a signal, or until ns have passed. True when signalled. The
 * timeout is kept to well under a millisecond, as os_sleep_ns is. */
bool os_wake_wait(os_wake_t *w, uint64_t ns);

#endif /* _OSAL_THREAD_H_ */
