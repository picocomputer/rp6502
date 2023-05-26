/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "cpu.h"
#include "ria.h"
#include "pico/stdlib.h"

static absolute_time_t resb_timer;
static bool is_running;

bool cpu_is_active()
{
    return is_running || gpio_get(RIA_RESB_PIN);
}

void cpu_run()
{
    is_running = true;
}

void cpu_stop()
{
    is_running = false;
    if (gpio_get(RIA_RESB_PIN))
    {
        gpio_put(RIA_RESB_PIN, false);
        resb_timer = delayed_by_us(get_absolute_time(),
                                   ria_get_reset_us());
    }
}

void cpu_init()
{
    // drive reset pin
    gpio_init(RIA_RESB_PIN);
    gpio_put(RIA_RESB_PIN, false);
    gpio_set_dir(RIA_RESB_PIN, true);

    // drive irq pin
    gpio_init(RIA_IRQB_PIN);
    gpio_put(RIA_IRQB_PIN, true);
    gpio_set_dir(RIA_IRQB_PIN, true);
}

void cpu_task()
{
    if (is_running && !gpio_get(RIA_RESB_PIN))
    {
        absolute_time_t now = get_absolute_time();
        if (absolute_time_diff_us(now, resb_timer) < 0)
            gpio_put(RIA_RESB_PIN, true);
    }
}
