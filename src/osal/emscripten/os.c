/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "osal/os.h"
#include "osal/emscripten/os.h"
#include <emscripten.h>
#include <time.h>

uint32_t os_random(void)
{
    struct timespec mono = {0}, real = {0};
    clock_gettime(CLOCK_MONOTONIC, &mono);
    clock_gettime(CLOCK_REALTIME, &real);
    uint64_t s = (uint64_t)mono.tv_nsec * 6364136223846793005ull +
                 (uint64_t)real.tv_sec * 1442695040888963407ull +
                 (uint64_t)real.tv_nsec + (uint64_t)(uintptr_t)&mono;
    return (uint32_t)(s ^ (s >> 32));
}

char *os_save_dir(void)
{
    return NULL;
}

/* Browser storage */

/* The JavaScript below writes these as numbers, so each value is fixed. */
enum
{
    JS_IDLE = 0,
    JS_RUNNING = 1,
    JS_OK = 2,
    JS_FAILED = 3,
    JS_ABSENT = 4,
};

EM_JS(void, js_syncfs, (int32_t *state), {
    FS.syncfs(false, (e) => { HEAP32[state >> 2] = e ? 3 : 2; });
});

EM_JS(void, js_estimate, (int32_t *state, double *quota, double *usage), {
    if (!navigator.storage?.estimate) {
        HEAP32[state >> 2] = 4;
        return;
    }
    navigator.storage.estimate().then((e) => {
        HEAPF64[quota >> 3] = e.quota;
        HEAPF64[usage >> 3] = e.usage;
        HEAP32[state >> 2] = 2;
    }, (e) => {
        console.error("navigator.storage.estimate:", e);
        HEAP32[state >> 2] = 3;
    });
});

static int32_t syncfs_state;
static bool syncfs_again;
static bool syncfs_failed;

static void syncfs_settle(void)
{
    if (syncfs_state == JS_IDLE || syncfs_state == JS_RUNNING)
        return;
    if (syncfs_state == JS_FAILED)
        syncfs_failed = true;
    syncfs_state = JS_IDLE;
    if (syncfs_again)
    {
        syncfs_again = false;
        syncfs_state = JS_RUNNING;
        js_syncfs(&syncfs_state);
    }
}

void os_syncfs_start(void)
{
    syncfs_settle();
    if (syncfs_state == JS_RUNNING)
        syncfs_again = true;
    else
    {
        syncfs_state = JS_RUNNING;
        js_syncfs(&syncfs_state);
    }
}

std_rw_result os_syncfs_poll(api_errno *err)
{
    syncfs_settle();
    if (syncfs_state == JS_RUNNING)
        return STD_PENDING;
    if (!syncfs_failed)
        return STD_OK;
    syncfs_failed = false;
    *err = API_EIO;
    return STD_ERROR;
}

static int32_t estimate_state;
static bool estimate_stale;
static double estimate_quota;
static double estimate_usage;

void os_estimate_stale(void)
{
    estimate_stale = true;
}

void os_estimate_start(void)
{
    if (estimate_state == JS_IDLE || (estimate_state == JS_OK && estimate_stale))
    {
        estimate_stale = false;
        estimate_state = JS_RUNNING;
        js_estimate(&estimate_state, &estimate_quota, &estimate_usage);
    }
}

std_rw_result os_estimate_poll(uint64_t *quota, uint64_t *usage, api_errno *err)
{
    switch (estimate_state)
    {
    case JS_RUNNING:
        return STD_PENDING;
    case JS_FAILED:
        estimate_state = JS_IDLE;
        *err = API_EIO;
        return STD_ERROR;
    case JS_ABSENT:
        estimate_state = JS_IDLE;
        *err = API_ENOSYS;
        return STD_ERROR;
    default:
        *quota = (uint64_t)estimate_quota;
        *usage = (uint64_t)estimate_usage;
        return STD_OK;
    }
}
