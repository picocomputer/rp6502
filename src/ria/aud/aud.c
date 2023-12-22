/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "aud/aud.h"
#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include <math.h>
#include <string.h>

static void (*aud_reclock_fn)(uint32_t sys_clk_khz);
static void (*aud_task_fn)();
static void (*aud_stop_fn)();

static void aud_null()
{
}

void aud_init(void)
{
    aud_stop_fn = aud_null;
    aud_stop();
}

void aud_stop(void)
{
    aud_stop_fn();
    aud_reclock_fn = aud_null;
    aud_task_fn = aud_null;
    aud_stop_fn = aud_null;
}

void aud_reclock(uint32_t sys_clk_khz)
{
    aud_reclock_fn(sys_clk_khz);
}

void aud_task(void)
{
    aud_task_fn();
}

void aud_setup(
    void (*start_fn)(void),
    void (*stop_fn)(void),
    void (*reclock_fn)(uint32_t sys_clk_khz),
    void (*task_fn)(void))
{
    if (stop_fn != aud_stop_fn)
    {
        aud_stop();
        start_fn();
        aud_stop_fn = stop_fn;
        aud_reclock_fn = reclock_fn;
        aud_task_fn = task_fn;
    }
}
