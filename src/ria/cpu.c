/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "cpu.h"
#include "api.h"
#include "ria.h"
#include "pico/stdlib.h"
#include "dev/com.h"
#include "cfg.h"
#include "hardware/clocks.h"

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
                                   cpu_get_reset_us());
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

void cpu_api_phi2()
{
    return api_return_ax(cfg_get_phi2_khz());
}

// Return calculated reset time. May be higher than requested
// to guarantee the 6502 gets two clock cycles during reset.
uint32_t cpu_get_reset_us()
{
    uint32_t reset_ms = cfg_get_reset_ms();
    uint32_t phi2_khz = cfg_get_phi2_khz();
    if (!reset_ms)
        return (2000000 / phi2_khz + 999) / 1000;
    if (phi2_khz == 1 && reset_ms == 1)
        return 2000;
    return reset_ms * 1000;
}

static void cpu_compute_phi2_clocks(uint32_t freq_khz, uint32_t *sys_clk_khz, uint16_t *clkdiv_int, uint8_t *clkdiv_frac)
{
    *sys_clk_khz = freq_khz * 30;
    if (*sys_clk_khz < 120 * 1000)
    {
        *sys_clk_khz = 120 * 1000;
        *clkdiv_int = *sys_clk_khz / 30 / freq_khz;
        *clkdiv_frac = (uint8_t)(*sys_clk_khz / 30.f / freq_khz - (float)*clkdiv_int) * (1u << 8u);
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

// Returns quantized actual frequency.
uint32_t cpu_validate_phi2_khz(uint32_t freq_khz)
{
    if (!freq_khz)
        freq_khz = 4000;
    uint32_t sys_clk_khz;
    uint16_t clkdiv_int;
    uint8_t clkdiv_frac;
    cpu_compute_phi2_clocks(freq_khz, &sys_clk_khz, &clkdiv_int, &clkdiv_frac);

    return sys_clk_khz / 30.f / (clkdiv_int + clkdiv_frac / 256.f);
}

bool cpu_set_phi2_khz(uint32_t phi2_khz)
{
    uint32_t sys_clk_khz;
    uint16_t clkdiv_int;
    uint8_t clkdiv_frac;
    cpu_compute_phi2_clocks(phi2_khz, &sys_clk_khz, &clkdiv_int, &clkdiv_frac);
    com_flush(); // TODO main_preclock
    bool ok = set_sys_clock_khz(sys_clk_khz, false);
    if (ok)
    {
        // TODO main_reclock(uint32_t phi2_khz, uint32_t sys_clk_khz, uint16_t clkdiv_int, uint8_t clkdiv_frac);
        com_init();
        pio_sm_set_clkdiv_int_frac(RIA_ACTION_PIO, RIA_ACTION_SM, clkdiv_int, clkdiv_frac);
        pio_sm_set_clkdiv_int_frac(RIA_WRITE_PIO, RIA_WRITE_SM, clkdiv_int, clkdiv_frac);
        pio_sm_set_clkdiv_int_frac(RIA_READ_PIO, RIA_READ_SM, clkdiv_int, clkdiv_frac);
        pio_sm_set_clkdiv_int_frac(RIA_PIX_PIO, RIA_PIX_SM, clkdiv_int, clkdiv_frac);
    }
    return ok;
}
