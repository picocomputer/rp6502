/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "api/api.h"
#include "mon/mon.h"
#include "str/str.h"
#include "sys/cfg.h"
#include "sys/cpu.h"
#include <pico/stdlib.h>
#include <hardware/clocks.h>
#include <hardware/vreg.h>

#if defined(DEBUG_RIA_SYS) || defined(DEBUG_RIA_SYS_CPU)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

static uint16_t cpu_phi2_khz;
static uint16_t cpu_phi2_khz_active;
static bool cpu_run_requested;
static absolute_time_t cpu_resb_timer;

// 6502 to RP2350 clock ratio is 1:32
static_assert(CPU_PHI2_MAX_KHZ <= CPU_RP2350_KHZ / 32);

static void cpu_change_phi2_khz(uint16_t freq_khz)
{
    if (freq_khz < CPU_PHI2_MIN_KHZ)
        freq_khz = CPU_PHI2_MIN_KHZ;
    if (freq_khz > CPU_PHI2_MAX_KHZ)
        freq_khz = CPU_PHI2_MAX_KHZ;
    float clkdiv = CPU_RP2350_KHZ / 32.f / freq_khz;
    uint16_t clkdiv_int = clkdiv;
    uint8_t clkdiv_frac = (clkdiv - clkdiv_int) * (1u << 8u);
    cpu_phi2_khz = CPU_RP2350_KHZ / 32.f / (clkdiv_int + clkdiv_frac / 256.f);
    if (cpu_phi2_khz_active == cpu_phi2_khz)
        return;
    cpu_phi2_khz_active = cpu_phi2_khz;
    main_reclock(clkdiv_int, clkdiv_frac);
}

void cpu_main(void)
{
    // The very first things main() does.
    gpio_init(CPU_RESB_PIN);
    gpio_put(CPU_RESB_PIN, false);
    gpio_set_dir(CPU_RESB_PIN, true);
    vreg_set_voltage(CPU_RP2350_VREG);
    set_sys_clock_khz(CPU_RP2350_KHZ, true);
}

void cpu_init(void)
{
    // Setting default
    if (!cpu_phi2_khz)
        cpu_change_phi2_khz(CPU_PHI2_DEFAULT);
}

void cpu_task(void)
{
    // Enforce minimum RESB time
    if (cpu_run_requested && !gpio_get(CPU_RESB_PIN))
    {
        absolute_time_t now = get_absolute_time();
        if (absolute_time_diff_us(now, cpu_resb_timer) < 0)
            gpio_put(CPU_RESB_PIN, true);
    }
}

void cpu_run(void)
{
    cpu_run_requested = true;
}

void cpu_stop(void)
{
    cpu_run_requested = false;
    gpio_put(CPU_RESB_PIN, false);
    cpu_resb_timer = delayed_by_us(get_absolute_time(), cpu_get_reset_us());
}

void cpu_reclock(void)
{
    cpu_resb_timer = delayed_by_us(get_absolute_time(), cpu_get_reset_us());
}

bool cpu_api_phi2(void)
{
    return api_return_ax(cpu_phi2_khz);
}

bool cpu_active(void)
{
    return cpu_run_requested;
}

uint32_t cpu_get_reset_us(void)
{
#ifndef RP6502_RESB_US
#define RP6502_RESB_US 0
#endif
    // If provided, use RP6502_RESB_US unless PHI2
    // speed needs longer for 2 clock cycles.
    // One extra microsecond to get ceil.
    uint32_t reset_us = 2000 / cpu_phi2_khz + 1;
    if (!RP6502_RESB_US)
        return reset_us;
    return RP6502_RESB_US < reset_us
               ? reset_us
               : RP6502_RESB_US;
}

void cpu_load_phi2_khz(const char *str, size_t len)
{
    str_parse_uint16(&str, &len, &cpu_phi2_khz);
    cpu_change_phi2_khz(cpu_phi2_khz);
}

bool cpu_set_phi2_khz(uint16_t phi2_khz)
{
    if (phi2_khz < CPU_PHI2_MIN_KHZ || phi2_khz > CPU_PHI2_MAX_KHZ)
        return false;
    uint16_t old_phi2_khz = cpu_phi2_khz;
    cpu_change_phi2_khz(phi2_khz);
    if (old_phi2_khz != cpu_phi2_khz)
        cfg_save();
    return true;
}

uint16_t cpu_get_phi2_khz(void)
{
    return cpu_phi2_khz;
}
