/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "str/str.h"
#include "sys/cfg.h"
#include "sys/cpu.h"
#include <pico/stdlib.h>
#include <hardware/clocks.h>
#include <hardware/sync.h>
#include <hardware/vreg.h>

#if defined(DEBUG_RIA_SYS) || defined(DEBUG_RIA_SYS_CPU)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

static uint16_t cpu_phi2_khz_run;
static uint16_t cpu_phi2_khz_set;
static volatile bool cpu_run_requested;
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
    uint16_t new_khz = CPU_RP2350_KHZ / 32.f / (clkdiv_int + clkdiv_frac / 256.f);
    if (cpu_phi2_khz_run == new_khz)
        return;
    cpu_phi2_khz_run = new_khz;
    main_reclock(clkdiv_int, clkdiv_frac);
}

void cpu_main(void)
{
    // The very first things main() does.
    gpio_init(CPU_RESB_PIN);
    gpio_put(CPU_RESB_PIN, false);
    gpio_set_dir(CPU_RESB_PIN, GPIO_OUT);
    vreg_set_voltage(CPU_RP2350_VREG);
    set_sys_clock_khz(CPU_RP2350_KHZ, true);
}

void cpu_init(void)
{
    // Setting default
    if (!cpu_phi2_khz_run)
    {
        cpu_change_phi2_khz(CPU_PHI2_DEFAULT);
        cpu_phi2_khz_set = cpu_phi2_khz_run;
    }
}

void cpu_task(void)
{
    if (!gpio_get(CPU_RESB_PIN))
    {
        // Acquire barrier pairs with the release DMB in cpu_stop().
        // If cpu_stop() lowered RESB before we observed it, we will
        // also observe cpu_run_requested=false and skip raising RESB.
        __dmb();
        if (cpu_run_requested)
        {
            // Enforce minimum RESB time
            if (time_reached(cpu_resb_timer))
                gpio_put(CPU_RESB_PIN, true);
        }
        else if (cpu_phi2_khz_run != cpu_phi2_khz_set)
        {
            cpu_change_phi2_khz(cpu_phi2_khz_set);
        }
    }
}

void cpu_run(void)
{
    cpu_run_requested = true;
}

void cpu_stop(void)
{
    // Called from both cpu0 and cpu1 (via act_loop). The DMB ensures
    // cpu_run_requested=false is visible to cpu0's cpu_task() before the GPIO
    // change is observable, preventing cpu_task() from raising RESB after we
    // lower it.
    cpu_run_requested = false;
    __dmb();
    gpio_put(CPU_RESB_PIN, false);
    cpu_resb_timer = make_timeout_time_us(cpu_get_reset_us());
}

void cpu_reclock(void)
{
    cpu_resb_timer = make_timeout_time_us(cpu_get_reset_us());
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
    uint32_t reset_us = 2000 / cpu_phi2_khz_run + 1;
    if (!RP6502_RESB_US)
        return reset_us;
    return RP6502_RESB_US < reset_us
               ? reset_us
               : RP6502_RESB_US;
}

void cpu_load_phi2_khz(const char *str)
{
    uint16_t phi2_khz;
    if (str_parse_uint16(&str, &phi2_khz) && phi2_khz)
    {
        cpu_change_phi2_khz(phi2_khz);
        cpu_phi2_khz_set = cpu_phi2_khz_run;
    }
}

void cpu_set_phi2_khz_run(uint16_t phi2_khz)
{
    cpu_change_phi2_khz(phi2_khz);
}

bool cpu_set_phi2_khz(uint16_t phi2_khz)
{
    if (phi2_khz < CPU_PHI2_MIN_KHZ || phi2_khz > CPU_PHI2_MAX_KHZ)
        return false;
    cpu_change_phi2_khz(phi2_khz);
    if (cpu_phi2_khz_set != cpu_phi2_khz_run)
    {
        cpu_phi2_khz_set = cpu_phi2_khz_run;
        cfg_save();
    }
    return true;
}

uint16_t cpu_get_phi2_khz(void)
{
    return cpu_phi2_khz_set;
}

uint16_t cpu_get_phi2_khz_run(void)
{
    return cpu_phi2_khz_run;
}
