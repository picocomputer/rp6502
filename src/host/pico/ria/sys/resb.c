/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * RESB on the board, where the 6502 runs on real silicon beside core 1 and
 * the line has a minimum hold time.
 */

#include "core/wdc/phi2.h"
#include "core/sys/config.h"
#include "ria/sys/cfg.h"
#include "ria/sys/resb.h"
#include <pico/stdlib.h>
#include <hardware/sync.h>

/* The ask, which is what resb_running answers: true for the whole hold, while
 * the pin is still low. */
static volatile bool run_requested;

/* Microseconds, not an absolute_time_t: resb_assert writes this from either
 * core and resb_task reads it on core 0, and a 64-bit store is two on this
 * part. A word is one, and the wrapping compare below is exact for any hold
 * shorter than half the 32-bit range -- this one is microseconds. */
static volatile uint32_t deadline_us;

void __in_flash("resb_init") resb_init(void)
{
    gpio_init(CPU_RESB_PIN);
    gpio_put(CPU_RESB_PIN, false);
    gpio_set_dir(CPU_RESB_PIN, GPIO_OUT);
}

void resb_assert(void)
{
    /* Called from both cores (core 1 via act_loop). The DMB ensures
     * run_requested=false is visible to core 0's resb_task before the GPIO
     * change is, so the task cannot raise the line after this lowered it. */
    run_requested = false;
    __dmb();
    gpio_put(CPU_RESB_PIN, false);
    deadline_us = time_us_32() + resb_get_reset_us();
}

void resb_release(void)
{
    run_requested = true;
}

bool resb_running(void)
{
    return run_requested;
}

void resb_reclock(void)
{
    deadline_us = time_us_32() + resb_get_reset_us();
}

/* Two things happen while the line is low, and only ever one of them: either a
 * run is waiting out the hold, or nothing is coming and the clock a finished
 * program left behind goes back to the configured rate. The restore is here
 * rather than in the ask because the ask runs on core 1, which is live on the
 * state machines a reclock reprograms. */
void resb_task(void)
{
    if (gpio_get(CPU_RESB_PIN))
        return;
    /* Acquire barrier pairs with the release DMB in resb_assert(). */
    __dmb();
    if (run_requested)
    {
        if ((int32_t)(time_us_32() - deadline_us) >= 0)
            gpio_put(CPU_RESB_PIN, true);
    }
    else
        phi2_set_khz_run(phi2_get_khz());
}

uint32_t resb_get_reset_us(void)
{
    /* Two PHI2 cycles, rounded up for margin. */
    return 2000 / phi2_get_khz_run() + 1;
}
