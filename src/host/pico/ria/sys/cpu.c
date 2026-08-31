/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ria/main.h"
#include "core/str/str.h"
#include "ria/sys/cfg.h"
#include "ria/sys/cpu.h"
#include "core/sys/config.h"
#include "core/rp2350.h"
#include <pico/stdlib.h>
#include <hardware/clocks.h>
#include <hardware/sync.h>
#include <hardware/vreg.h>

#if defined(DEBUG_SYS) || defined(DEBUG_SYS_CPU)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

static uint16_t cpu_phi2_khz_run;
static volatile bool cpu_run_requested;
/* Microseconds, not an absolute_time_t: cpu_stop writes this from either core
 * and cpu_task reads it on core 0, and a 64-bit store is two on this part. A
 * word is one, and the wrapping compare below is exact for any hold shorter
 * than half the 32-bit range -- this one is microseconds. */
static volatile uint32_t cpu_resb_deadline_us;

// 6502 to RP2350 clock ratio is 1:32
static_assert(CPU_PHI2_MAX_KHZ <= SYS_RP2350_KHZ / 32);

/* The divider this rate lands on, and the rate that divider actually gives.
 * Pure arithmetic on the request -- no hardware is read -- which is what lets
 * the check normalize a value without reclocking anything. */
static uint16_t cpu_quantize(uint16_t freq_khz, uint16_t *div_int, uint8_t *div_frac)
{
    if (freq_khz < CPU_PHI2_MIN_KHZ)
        freq_khz = CPU_PHI2_MIN_KHZ;
    if (freq_khz > CPU_PHI2_MAX_KHZ)
        freq_khz = CPU_PHI2_MAX_KHZ;
    float clkdiv = SYS_RP2350_KHZ / 32.f / freq_khz;
    uint16_t clkdiv_int = clkdiv;
    uint8_t clkdiv_frac = (clkdiv - clkdiv_int) * (1u << 8u);
    if (div_int)
        *div_int = clkdiv_int;
    if (div_frac)
        *div_frac = clkdiv_frac;
    return SYS_RP2350_KHZ / 32.f / (clkdiv_int + clkdiv_frac / 256.f);
}

uint16_t cpu_quantize_phi2_khz(uint16_t freq_khz)
{
    return cpu_quantize(freq_khz, NULL, NULL);
}

static void cpu_change_phi2_khz(uint16_t freq_khz)
{
    uint16_t clkdiv_int;
    uint8_t clkdiv_frac;
    uint16_t new_khz = cpu_quantize(freq_khz, &clkdiv_int, &clkdiv_frac);
    if (cpu_phi2_khz_run == new_khz)
        return;
    cpu_phi2_khz_run = new_khz;
    main_reclock(clkdiv_int, clkdiv_frac);
}

void __in_flash("cpu_init") cpu_init(void)
{
    // Hold the 6502 in reset.
    gpio_init(CPU_RESB_PIN);
    gpio_put(CPU_RESB_PIN, false);
    gpio_set_dir(CPU_RESB_PIN, GPIO_OUT);

    cpu_apply_phi2_khz(cpu_get_phi2_khz(), true);
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
            if ((int32_t)(time_us_32() - cpu_resb_deadline_us) >= 0)
                gpio_put(CPU_RESB_PIN, true);
        }
        else if (cpu_phi2_khz_run != cpu_get_phi2_khz())
        {
            cpu_change_phi2_khz(cpu_get_phi2_khz());
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
    cpu_resb_deadline_us = time_us_32() + cpu_get_reset_us();
}

void cpu_reclock(void)
{
    cpu_resb_deadline_us = time_us_32() + cpu_get_reset_us();
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
    // Always round up one microsecond for safety margin.
    uint32_t reset_us = 2000 / cpu_phi2_khz_run + 1;
    if (!RP6502_RESB_US)
        return reset_us;
    return RP6502_RESB_US < reset_us
               ? reset_us
               : RP6502_RESB_US;
}

void cpu_set_phi2_khz_run(uint16_t phi2_khz)
{
    cpu_change_phi2_khz(phi2_khz);
}

/* What the file keeps is the rate the divider can actually give, so a
 * config written here reads back as the machine runs. */
bool cpu_check_phi2_khz(uint16_t *v)
{
    if (*v < CPU_PHI2_MIN_KHZ || *v > CPU_PHI2_MAX_KHZ)
        return false;
    *v = cpu_quantize_phi2_khz(*v);
    return true;
}

void cpu_apply_phi2_khz(uint16_t phi2_khz, bool changed)
{
    (void)changed;
    cpu_change_phi2_khz(phi2_khz);
}

/* SET's line for this row. */
int cpu_phi2_response(char *buf, size_t buf_size, int state, unsigned width)
{
    (void)state;
    (void)width;
    snprintf(buf, buf_size, STR_SET_PHI2_RESPONSE, cpu_get_phi2_khz());
    return -1;
}

uint16_t cpu_get_phi2_khz_run(void)
{
    return cpu_phi2_khz_run;
}
