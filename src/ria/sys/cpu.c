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
#include "hardware/clocks.h"

static bool cpu_run_requested;
static absolute_time_t cpu_resb_timer;

volatile int cpu_rx_char;
static size_t cpu_rx_tail;
static size_t cpu_rx_head;
static uint8_t cpu_rx_buf[32];
#define CPU_RX_BUF(pos) cpu_rx_buf[(pos) & 0x1F]

static bool cpu_readline_active;
static const char *cpu_readline_buf;
static bool cpu_readline_needs_nl;
static size_t cpu_readline_pos;
static size_t cpu_readline_length;
static size_t cpu_str_length = 254;
static uint32_t cpu_ctrl_bits;

void cpu_init(void)
{
    // drive reset pin
    gpio_init(CPU_RESB_PIN);
    gpio_put(CPU_RESB_PIN, false);
    gpio_set_dir(CPU_RESB_PIN, true);
}

void cpu_reclock(void)
{
    if (!gpio_get(CPU_RESB_PIN))
        cpu_resb_timer = delayed_by_us(get_absolute_time(), cpu_get_reset_us());
}

static int cpu_caps(int ch)
{
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
    return ch;
}

static int cpu_getchar_fifo(void)
{
    if (&CPU_RX_BUF(cpu_rx_head) != &CPU_RX_BUF(cpu_rx_tail))
        return CPU_RX_BUF(++cpu_rx_tail);
    return -1;
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

    // Move UART FIFO into action loop
    if (cpu_rx_char < 0)
        cpu_rx_char = cpu_caps(cpu_getchar_fifo());
}

static void clear_com_rx_fifo()
{
    cpu_rx_char = -1;
    cpu_rx_tail = cpu_rx_head = 0;
}

void cpu_run(void)
{
    cpu_run_requested = true;
    clear_com_rx_fifo();
}

void cpu_stop(void)
{
    clear_com_rx_fifo();
    cpu_readline_active = false;
    cpu_readline_needs_nl = false;
    cpu_readline_pos = 0;
    cpu_readline_length = 0;
    cpu_str_length = 254;
    cpu_ctrl_bits = 0;

    cpu_run_requested = false;
    if (gpio_get(CPU_RESB_PIN))
    {
        gpio_put(CPU_RESB_PIN, false);
        cpu_resb_timer = delayed_by_us(get_absolute_time(),
                                       cpu_get_reset_us());
    }
}

bool cpu_active(void)
{
    return cpu_run_requested;
}

void cpu_api_phi2(void)
{
    return api_return_ax(cfg_get_phi2_khz());
}

uint32_t cpu_get_reset_us(void)
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
    if (&CPU_RX_BUF(cpu_rx_head + 1) != &CPU_RX_BUF(cpu_rx_tail))
        CPU_RX_BUF(++cpu_rx_head) = ch;
}

// Used by std.c to get stdin destined for the CPU.
// Mixing RIA register input with read() calls isn't perfect.
// Even with a mutex, nulls may appear from RIA register.
int cpu_getchar(void)
{
    // Steal char from RIA register
    if (REGS(0xFFE0) & 0b01000000)
    {
        REGS(0xFFE0) &= ~0b01000000;
        int ch = REGS(0xFFE2);
        // Replace char with null
        REGS(0xFFE2) = 0;
        return cpu_caps(ch);
    }
    // Steal char from action loop queue
    if (cpu_rx_char >= 0)
    {
        int ch = cpu_rx_char;
        cpu_rx_char = -1;
        return cpu_caps(ch);
    }
    // Get char from FIFO
    int ch = cpu_getchar_fifo();
    // Get char from UART
    if (ch < 0)
        ch = getchar_timeout_us(0);
    return cpu_caps(ch);
}

static void cpu_enter(bool timeout, const char *buf, size_t length)
{
    (void)timeout;
    assert(!timeout);
    cpu_readline_active = false;
    cpu_readline_buf = buf;
    cpu_readline_pos = 0;
    cpu_readline_length = length;
    cpu_readline_needs_nl = true;
}

void cpu_stdin_request(void)
{
    if (!cpu_readline_needs_nl)
    {
        cpu_readline_active = true;
        com_read_line(0, cpu_enter, cpu_str_length + 1, cpu_ctrl_bits);
    }
}

bool cpu_stdin_ready(void)
{
    return !cpu_readline_active;
}

size_t cpu_stdin_read(uint8_t *buf, size_t count)
{
    size_t i;
    for (i = 0; i < count && cpu_readline_pos < cpu_readline_length; i++)
        buf[i] = cpu_readline_buf[cpu_readline_pos++];
    if (i < count && cpu_readline_needs_nl)
    {
        buf[i++] = '\n';
        cpu_readline_needs_nl = false;
    }
    return i;
}

void cpu_api_stdin_opt(void)
{
    uint8_t str_length = API_A;
    uint32_t ctrl_bits;
    if (!api_pop_uint32_end(&ctrl_bits))
        return api_return_errno(API_EINVAL);
    cpu_str_length = str_length;
    cpu_ctrl_bits = ctrl_bits;
    return api_return_ax(0);
}
