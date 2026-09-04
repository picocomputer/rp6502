/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ria/sys/rp2350.h"
#include "core/str/str.h"
#include "core/sys/config.h"
#include "ria/sys/cfg.h"
#include "ria/sys/phi2.h"
#include "ria/sys/pix.h"
#include "ria/sys/resb.h"
#include "ria/sys/ria.h"
#include <pico/stdlib.h>

static uint16_t phi2_khz_run;

// 6502 to RP2350 clock ratio is 1:32
static_assert(PHI2_MAX_KHZ <= SYS_RP2350_KHZ / 32);

/* The divider this rate lands on, and the rate that divider actually gives.
 * Pure arithmetic on the request -- no hardware is read -- which is what lets
 * the check normalize a value without reclocking anything. */
static uint16_t quantize(uint16_t freq_khz, uint16_t *div_int, uint8_t *div_frac)
{
    if (freq_khz < PHI2_MIN_KHZ)
        freq_khz = PHI2_MIN_KHZ;
    if (freq_khz > PHI2_MAX_KHZ)
        freq_khz = PHI2_MAX_KHZ;
    float clkdiv = SYS_RP2350_KHZ / 32.f / freq_khz;
    uint16_t clkdiv_int = clkdiv;
    uint8_t clkdiv_frac = (clkdiv - clkdiv_int) * (1u << 8u);
    if (div_int)
        *div_int = clkdiv_int;
    if (div_frac)
        *div_frac = clkdiv_frac;
    return SYS_RP2350_KHZ / 32.f / (clkdiv_int + clkdiv_frac / 256.f);
}

static uint16_t phi2_quantize_khz(uint16_t freq_khz)
{
    return quantize(freq_khz, NULL, NULL);
}

/* The one place the divider moves, and the fan-out to everything that runs
 * off it: the two state machines that divide from the same clock, and the
 * reset hold, whose minimum is two PHI2 cycles of whatever the rate now is. */
static void change(uint16_t freq_khz)
{
    uint16_t clkdiv_int;
    uint8_t clkdiv_frac;
    uint16_t new_khz = quantize(freq_khz, &clkdiv_int, &clkdiv_frac);
    if (phi2_khz_run == new_khz)
        return;
    phi2_khz_run = new_khz;
    resb_reclock();
    ria_reclock(clkdiv_int, clkdiv_frac);
    pix_reclock(clkdiv_int, clkdiv_frac);
}

void __in_flash("phi2_init") phi2_init(void)
{
    phi2_apply_khz(phi2_get_khz(), true);
}

void phi2_set_khz_run(uint16_t phi2_khz)
{
    change(phi2_khz);
}

/* What the store keeps is the rate the divider can actually give, so a
 * config written here reads back as the machine runs. */
bool phi2_check_khz(uint16_t *v)
{
    if (*v < PHI2_MIN_KHZ || *v > PHI2_MAX_KHZ)
        return false;
    *v = phi2_quantize_khz(*v);
    return true;
}

void phi2_apply_khz(uint16_t phi2_khz, bool changed)
{
    (void)changed;
    change(phi2_khz);
}

/* SET's line for this row. */
int phi2_response(char *buf, size_t buf_size, int state, unsigned width)
{
    (void)state;
    (void)width;
    snprintf(buf, buf_size, STR_SET_PHI2_RESPONSE, phi2_get_khz());
    return -1;
}

uint16_t phi2_get_khz_run(void)
{
    return phi2_khz_run;
}
