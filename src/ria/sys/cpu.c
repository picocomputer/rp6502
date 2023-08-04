/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "api/api.h"
#include "sys/cfg.h"
#include "sys/com.h"
#include "sys/cpu.h"
#include "pico/stdlib.h"

static bool cpu_run_requested;
static absolute_time_t cpu_resb_timer;
volatile int cpu_rx_char;
static size_t cpu_rx_head;
static size_t cpu_rx_tail;
static uint8_t cpu_rx_buf[32];
#define CPU_RX_BUF(pos) cpu_rx_buf[(pos)&0x1F]

void cpu_init()
{
    // drive reset pin
    gpio_init(CPU_RESB_PIN);
    gpio_put(CPU_RESB_PIN, false);
    gpio_set_dir(CPU_RESB_PIN, true);

    // drive irq pin
    gpio_init(CPU_IRQB_PIN);
    gpio_put(CPU_IRQB_PIN, true);
    gpio_set_dir(CPU_IRQB_PIN, true);
}

void cpu_reclock()
{
    if (!gpio_get(CPU_RESB_PIN))
        cpu_resb_timer = delayed_by_us(get_absolute_time(), cpu_get_reset_us());
}

void cpu_task()
{
    // Enforce minimum RESB time
    if (cpu_run_requested && !gpio_get(CPU_RESB_PIN))
    {
        absolute_time_t now = get_absolute_time();
        if (absolute_time_diff_us(now, cpu_resb_timer) < 0)
            gpio_put(CPU_RESB_PIN, true);
    }

    // Move UART FIFO into action loop
    if (cpu_rx_char < 0 && &CPU_RX_BUF(cpu_rx_tail) != &CPU_RX_BUF(cpu_rx_head))
    {
        int ch = CPU_RX_BUF(++cpu_rx_head);
        switch (cfg_get_caps())
        {
        case 1:
            if (ch >= 'A' && ch <= 'Z')
            {
                ch += 32;
                break;
            }
            // fall through
        case 2:
            if (ch >= 'a' && ch <= 'z')
                ch -= 32;
        }
        cpu_rx_char = ch;
    }
}

static void clear_com_rx_fifo()
{
    cpu_rx_char = -1;
    cpu_rx_head = cpu_rx_tail = 0;
}

void cpu_run()
{
    cpu_run_requested = true;
    clear_com_rx_fifo();
}

void cpu_stop()
{
    clear_com_rx_fifo();
    cpu_run_requested = false;
    if (gpio_get(CPU_RESB_PIN))
    {
        gpio_put(CPU_RESB_PIN, false);
        cpu_resb_timer = delayed_by_us(get_absolute_time(),
                                       cpu_get_reset_us());
    }
}

bool cpu_active()
{
    return cpu_run_requested;
}

void cpu_api_phi2()
{
    return api_return_ax(cfg_get_phi2_khz());
}

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

static void cpu_compute_phi2_clocks(uint32_t freq_khz,
                                    uint32_t *sys_clk_khz,
                                    uint16_t *clkdiv_int,
                                    uint8_t *clkdiv_frac)
{
    *sys_clk_khz = freq_khz * 30;
    if (*sys_clk_khz < 120 * 1000)
    {
        *sys_clk_khz = 120 * 1000;
        float clkdiv = 120000.f / 30.f / freq_khz;
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
    com_flush();
    bool ok = set_sys_clock_khz(sys_clk_khz, false);
    if (ok)
        main_reclock(sys_clk_khz, clkdiv_int, clkdiv_frac);
    return ok;
}

void cpu_com_rx(uint8_t ch)
{
    // discarding overflow
    if (&CPU_RX_BUF(cpu_rx_tail + 1) != &CPU_RX_BUF(cpu_rx_head))
        CPU_RX_BUF(++cpu_rx_tail) = ch;
}
