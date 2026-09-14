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

static absolute_time_t hold_end;

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
}

/* The hold is timed from the ask, not from the assert that lowered the line:
 * the line has been low at least that long already, and the ask is the only
 * moment the hold is consulted. A stamp taken at the assert would be as old
 * as the idle before the ask, and written from either core. */
void resb_release(void)
{
    hold_end = make_timeout_time_us(resb_get_reset_us());
    run_requested = true;
}

bool resb_running(void)
{
    return run_requested;
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
        if (time_reached(hold_end))
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
