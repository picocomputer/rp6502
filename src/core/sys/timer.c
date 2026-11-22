/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/timer.h"

#include "host/host.h"
#include "osal/os.h"
#include "machine.h"

uint64_t HOST_IN_RAM_FUNC timer_ns(void)
{
    return os_mono_ns();
}

timer_deadline_t HOST_IN_RAM_FUNC timer_in_us(uint64_t us)
{
    return os_mono_ns() + us * 1000;
}

timer_deadline_t HOST_IN_RAM_FUNC timer_in_ms(uint64_t ms)
{
    return os_mono_ns() + ms * 1000000;
}

bool HOST_IN_RAM_FUNC timer_passed(timer_deadline_t d)
{
    return (int64_t)(os_mono_ns() - d) >= 0;
}

timer_mach_t HOST_IN_RAM_FUNC timer_mach_in_us(uint64_t us)
{
    return host_clock_us() + us;
}

timer_mach_t HOST_IN_RAM_FUNC timer_mach_in_ms(uint64_t ms)
{
    return host_clock_us() + ms * 1000;
}

bool HOST_IN_RAM_FUNC timer_mach_passed(timer_mach_t d)
{
    return (int64_t)(host_clock_us() - d) >= 0;
}
