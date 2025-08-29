/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef RP6502_RIA_W
#include "net/cyw.h"
void cyw_task() {}
void cyw_pre_reclock() {}
void cyw_post_reclock(uint32_t) {}
void cyw_reset_radio() {}
#else

#include "net/ble.h"
#include "net/cyw.h"
#include "net/wfi.h"
#include "sys/cfg.h"
#include <pico/cyw43_arch.h>
#include <pico/cyw43_driver.h>
#include <pico/stdio.h>
#include <hardware/clocks.h>

#if defined(DEBUG_RIA_NET) || defined(DEBUG_RIA_NET_CYW)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// These are from cyw43_arch.h
// Change the help if you change these
static const char COUNTRY_CODES[] = {
    'A', 'U', // AUSTRALIA
    'A', 'T', // AUSTRIA
    'B', 'E', // BELGIUM
    'B', 'R', // BRAZIL
    'C', 'A', // CANADA
    'C', 'L', // CHILE
    'C', 'N', // CHINA
    'C', 'O', // COLOMBIA
    'C', 'Z', // CZECH_REPUBLIC
    'D', 'K', // DENMARK
    'E', 'E', // ESTONIA
    'F', 'I', // FINLAND
    'F', 'R', // FRANCE
    'D', 'E', // GERMANY
    'G', 'R', // GREECE
    'H', 'K', // HONG_KONG
    'H', 'U', // HUNGARY
    'I', 'S', // ICELAND
    'I', 'N', // INDIA
    'I', 'L', // ISRAEL
    'I', 'T', // ITALY
    'J', 'P', // JAPAN
    'K', 'E', // KENYA
    'L', 'V', // LATVIA
    'L', 'I', // LIECHTENSTEIN
    'L', 'T', // LITHUANIA
    'L', 'U', // LUXEMBOURG
    'M', 'Y', // MALAYSIA
    'M', 'T', // MALTA
    'M', 'X', // MEXICO
    'N', 'L', // NETHERLANDS
    'N', 'Z', // NEW_ZEALAND
    'N', 'G', // NIGERIA
    'N', 'O', // NORWAY
    'P', 'E', // PERU
    'P', 'H', // PHILIPPINES
    'P', 'L', // POLAND
    'P', 'T', // PORTUGAL
    'S', 'G', // SINGAPORE
    'S', 'K', // SLOVAKIA
    'S', 'I', // SLOVENIA
    'Z', 'A', // SOUTH_AFRICA
    'K', 'R', // SOUTH_KOREA
    'E', 'S', // SPAIN
    'S', 'E', // SWEDEN
    'C', 'H', // SWITZERLAND
    'T', 'W', // TAIWAN
    'T', 'H', // THAILAND
    'T', 'R', // TURKEY
    'G', 'B', // UK
    'U', 'S', // USA
};

bool cyw_led_status;
bool cyw_led_requested;
bool cyw_initialized;

bool cyw_validate_country_code(char *cc)
{
    if (!cc[0] || !cc[1] || cc[2] != 0)
        return false;
    for (size_t i = 0; i < sizeof(COUNTRY_CODES); i += 2)
        if (cc[0] == COUNTRY_CODES[i] && cc[1] == COUNTRY_CODES[i + 1])
            return true;
    return false;
}

void cyw_reset_radio(void)
{
    // We have to shut down to reclock so use that code
    uint32_t sys_clk_khz = clock_get_hz(clk_sys) / 1000;
    cyw_pre_reclock();
    cyw_post_reclock(sys_clk_khz);
}

void cyw_task(void)
{
    if (cyw_led_requested != cyw_led_status)
    {
        cyw_led_status = cyw_led_requested;
#ifdef CYW43_WL_GPIO_LED_PIN
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, cyw_led_status);
#endif
    }
    cyw43_arch_poll();
}

void cyw_led(bool ison)
{
    cyw_led_requested = ison;
}

void cyw_pre_reclock(void)
{
    wfi_shutdown();
    ble_shutdown();
    if (cyw_initialized)
        cyw43_arch_deinit();
    cyw_initialized = false;
}

void cyw_post_reclock(uint32_t sys_clk_khz)
{
    // CYW43439 datasheet says 50MHz for SPI.
    // The Raspberry Pi SDK only provides for a 2,0 divider,
    // which is 75MHz for a non-overclocked 150MHz system clock.
    // It easily runs 85MHz+ so we push it to 66MHz.
    if (sys_clk_khz > 198000)
        cyw43_set_pio_clkdiv_int_frac8(4, 0);
    else if (sys_clk_khz > 132000)
        cyw43_set_pio_clkdiv_int_frac8(3, 0);
    else
        cyw43_set_pio_clkdiv_int_frac8(2, 0);

    uint32_t cyw_country_code = CYW43_COUNTRY_WORLDWIDE;
    const char *cc = cfg_get_rfcc();
    if (strlen(cc) == 2)
        cyw_country_code = CYW43_COUNTRY(cc[0], cc[1], 0);
    if (cyw43_arch_init_with_country(cyw_country_code))
    {
        puts("?CYW43 failed to init");
    }
    else
    {
        // cyw43_arch is full of blocking functions.
        // This seems to block only once after cyw43_arch_init.
        cyw_led_status = cyw_led_requested;
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, cyw_led_status);
        cyw_initialized = true;
    }
}

#endif /* RP6502_RIA_W */
