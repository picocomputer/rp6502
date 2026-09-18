/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/wdc/phi2.h"
#include "core/sys/config.h"
#include "ria/sys/cfg.h"
#include "ria/sys/resb.h"
#include <pico/stdlib.h>
#include <hardware/sync.h>

static volatile bool release_pending;

static absolute_time_t hold_end;

void __in_flash("resb_init") resb_init(void)
{
    gpio_init(CPU_RESB_PIN);
    gpio_put(CPU_RESB_PIN, false);
    gpio_set_dir(CPU_RESB_PIN, GPIO_OUT);
}

void resb_assert(void)
{
    /* resb_assert is called on both cores, on core 1 through act_loop. The
     * barrier makes release_pending false visible to resb_task on core 0
     * before RESB goes low, so resb_task cannot raise RESB after this call has
     * lowered it. */
    release_pending = false;
    __dmb();
    gpio_put(CPU_RESB_PIN, false);
}

/* The time RESB must stay low is measured from here rather than from
 * resb_assert, because resb_assert also runs on core 1 while resb_task reads
 * hold_end on core 0. resb_release runs only on core 0, so hold_end is written
 * and read on the same core. RESB has been low at least since the assert, so
 * measuring from the release never cuts that time short. */
void resb_release(void)
{
    hold_end = make_timeout_time_us(resb_get_reset_us());
    release_pending = true;
}

/* The PHI2 rate a program set is put back to the configured rate here rather
 * than in resb_assert, because resb_assert also runs on core 1 and every other
 * reclock runs on core 0 without a lock. */
void resb_task(void)
{
    if (gpio_get(CPU_RESB_PIN))
        return;
    /* This barrier pairs with the one in resb_assert, so release_pending is
     * read after the pin level. */
    __dmb();
    if (release_pending)
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
