/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "api/api.h"
#include "sys/cfg.h"
#include "sys/cpu.h"
#include <pico/stdlib.h>
#include <hardware/clocks.h>

#if defined(DEBUG_RIA_SYS) || defined(DEBUG_RIA_SYS_CPU)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

static bool cpu_run_requested;
static absolute_time_t cpu_resb_timer;

void cpu_init(void)
{
    // drive reset pin
    gpio_init(CPU_RESB_PIN);
    gpio_put(CPU_RESB_PIN, false);
    gpio_set_dir(CPU_RESB_PIN, true);
}

void cpu_post_reclock(void)
{
    if (!gpio_get(CPU_RESB_PIN))
        cpu_resb_timer = delayed_by_us(get_absolute_time(), cpu_get_reset_us());
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
    cpu_resb_timer = delayed_by_us(get_absolute_time(),
                                   cpu_get_reset_us());
}

bool cpu_active(void)
{
    return cpu_run_requested;
}

bool cpu_api_phi2(void)
{
    return api_return_ax(cfg_get_phi2_khz());
}

uint32_t cpu_get_reset_us(void)
{
#ifndef RP6502_RESB_US
#define RP6502_RESB_US 0
#endif
    // If provided, use RP6502_RESB_US unless PHI2
    // speed needs longer for 2 clock cycles.
    // One extra microsecond to get ceil.
    uint32_t reset_us = 2000 / cfg_get_phi2_khz() + 1;
    if (!RP6502_RESB_US)
        return reset_us;
    return RP6502_RESB_US < reset_us
               ? reset_us
               : RP6502_RESB_US;
}

static void cpu_compute_phi2_clocks(uint32_t freq_khz,
                                    uint32_t *sys_clk_khz,
                                    uint16_t *clkdiv_int,
                                    uint8_t *clkdiv_frac)
{
    *sys_clk_khz = freq_khz * 32;
    if (*sys_clk_khz < 128 * 1000)
    {
        *sys_clk_khz = 128 * 1000;
        float clkdiv = 128000.f / 32.f / freq_khz;
        *clkdiv_int = clkdiv;
        *clkdiv_frac = (clkdiv - *clkdiv_int) * (1u << 8u);
    }
    else
    {
        *clkdiv_int = 1;
        *clkdiv_frac = 0;
        uint vco, postdiv1, postdiv2;
        while (!check_sys_clock_khz(*sys_clk_khz, &vco, &postdiv1, &postdiv2))
            *sys_clk_khz += 1;
    }
}

uint32_t cpu_validate_phi2_khz(uint32_t freq_khz)
{
    if (!freq_khz)
        freq_khz = CPU_PHI2_DEFAULT;
    if (freq_khz < CPU_PHI2_MIN_KHZ)
        freq_khz = CPU_PHI2_MIN_KHZ;
    if (freq_khz > CPU_PHI2_MAX_KHZ)
        freq_khz = CPU_PHI2_MAX_KHZ;
    uint32_t sys_clk_khz;
    uint16_t clkdiv_int;
    uint8_t clkdiv_frac;
    cpu_compute_phi2_clocks(freq_khz, &sys_clk_khz, &clkdiv_int, &clkdiv_frac);
    return sys_clk_khz / 32.f / (clkdiv_int + clkdiv_frac / 256.f);
}

bool cpu_set_phi2_khz(uint32_t phi2_khz)
{
    uint32_t sys_clk_khz;
    uint16_t clkdiv_int;
    uint8_t clkdiv_frac;
    cpu_compute_phi2_clocks(phi2_khz, &sys_clk_khz, &clkdiv_int, &clkdiv_frac);
    main_pre_reclock(sys_clk_khz, clkdiv_int, clkdiv_frac);
    bool ok = set_sys_clock_khz(sys_clk_khz, false);
    if (ok)
        main_post_reclock(sys_clk_khz, clkdiv_int, clkdiv_frac);
    return ok;
}
