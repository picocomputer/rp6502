/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * See timer.h.
 */

#include "core/sys/timer.h"

#include "osal/os.h"

uint64_t timer_ns(void)
{
    return os_mono_ns();
}

timer_deadline_t timer_in_us(uint64_t us)
{
    return os_mono_ns() + us * 1000;
}

timer_deadline_t timer_in_ms(uint64_t ms)
{
    return os_mono_ns() + ms * 1000000;
}

bool timer_passed(timer_deadline_t d)
{
    return (int64_t)(os_mono_ns() - d) >= 0;
}
