/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef RP6502_RIA_W
#include "net/cyw.h"
void cyw_init(void) {}
void cyw_task(void) {}
#else

#include "mon/mon.h"
#include "net/ble.h"
#include "net/cyw.h"
#include "net/wfi.h"
#include "str/str.h"
#include "sys/cfg.h"
#include "sys/cpu.h"
#include <pico/cyw43_arch.h>
#include <pico/cyw43_driver.h>

#if defined(DEBUG_RIA_NET) || defined(DEBUG_RIA_NET_CYW)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// These are from cyw43_arch.h
#define CYW_CC_X                  \
    X(AU, "AU", "Australia")      \
    X(AT, "AT", "Austria")        \
    X(BE, "BE", "Belgium")        \
    X(BR, "BR", "Brazil")         \
    X(CA, "CA", "Canada")         \
    X(CL, "CL", "Chile")          \
    X(CN, "CN", "China")          \
    X(CO, "CO", "Colombia")       \
    X(CZ, "CZ", "Czech Republic") \
    X(DK, "DK", "Denmark")        \
    X(EE, "EE", "Estonia")        \
    X(FI, "FI", "Finland")        \
    X(FR, "FR", "France")         \
    X(DE, "DE", "Germany")        \
    X(GR, "GR", "Greece")         \
    X(HK, "HK", "Hong Kong")      \
    X(HU, "HU", "Hungary")        \
    X(IS, "IS", "Iceland")        \
    X(IN, "IN", "India")          \
    X(IL, "IL", "Israel")         \
    X(IT, "IT", "Italy")          \
    X(JP, "JP", "Japan")          \
    X(KE, "KE", "Kenya")          \
    X(LV, "LV", "Latvia")         \
    X(LI, "LI", "Liechtenstein")  \
    X(LT, "LT", "Lithuania")      \
    X(LU, "LU", "Luxembourg")     \
    X(MY, "MY", "Malaysia")       \
    X(MT, "MT", "Malta")          \
    X(MX, "MX", "Mexico")         \
    X(NL, "NL", "Netherlands")    \
    X(NZ, "NZ", "New Zealand")    \
    X(NG, "NG", "Nigeria")        \
    X(NO, "NO", "Norway")         \
    X(PE, "PE", "Peru")           \
    X(PH, "PH", "Philippines")    \
    X(PL, "PL", "Poland")         \
    X(PT, "PT", "Portugal")       \
    X(SG, "SG", "Singapore")      \
    X(SK, "SK", "Slovakia")       \
    X(SI, "SI", "Slovenia")       \
    X(ZA, "ZA", "South Africa")   \
    X(KR, "KR", "South Korea")    \
    X(ES, "ES", "Spain")          \
    X(SE, "SE", "Sweden")         \
    X(CH, "CH", "Switzerland")    \
    X(TW, "TW", "Taiwan")         \
    X(TH, "TH", "Thailand")       \
    X(TR, "TR", "Turkey")         \
    X(GB, "GB", "United Kingdom") \
    X(US, "US", "United States")

#define X(suffix, abbr, name)                         \
    static const char __in_flash("cyw_country_codes") \
        CYW_COUNTRY_ABBR_##suffix[] = abbr;           \
    static const char __in_flash("cyw_country_codes") \
        CYW_COUNTRY_NAME_##suffix[] = name;
CYW_CC_X
#undef X

#define X(suffix, abbr, name) \
    CYW_COUNTRY_ABBR_##suffix,
static const char *__in_flash("cyw_country_abbr")
    cyw_country_abbr[] = {CYW_CC_X};
#undef X

#define X(suffix, abbr, name) \
    CYW_COUNTRY_NAME_##suffix,
static const char *__in_flash("cyw_country_name")
    cyw_country_name[] = {CYW_CC_X};
#undef X

#define CYW_COUNTRY_COUNT (sizeof(cyw_country_abbr) / sizeof(cyw_country_abbr)[0])

static uint8_t cyw_rf_enable = 1;
static int cyw_country = -1;
static bool cyw_led_status;
static bool cyw_led_requested;

static int cyw_lookup_country(const char *cc)
{
    for (size_t i = 0; i < CYW_COUNTRY_COUNT; i++)
    {
        if (!strcasecmp(cc, cyw_country_abbr[i]))
            return i;
    }
    return -1;
}

static void cyw_reset_radio(void)
{
    wfi_shutdown();
    ble_shutdown();
    cyw43_arch_deinit();
    cyw_init();
}

void cyw_task(void)
{
    if (cyw_led_requested != cyw_led_status)
    {
        cyw_led_status = cyw_led_requested;
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, cyw_led_status);
    }
    cyw43_arch_poll();
}

void cyw_led_set(bool on)
{
    cyw_led_requested = on;
}

void cyw_init(void)
{
    // CYW43439 datasheet says 50MHz for SPI.
    // The Raspberry Pi SDK only provides for a 2,0 divider,
    // which is 75MHz for a non-overclocked 150MHz system clock.
    // It easily runs 85MHz+ so we push it to 66MHz.
    if (CPU_RP2350_KHZ > 198000)
        cyw43_set_pio_clkdiv_int_frac8(4, 0);
    else if (CPU_RP2350_KHZ > 132000)
        cyw43_set_pio_clkdiv_int_frac8(3, 0);
    else
        cyw43_set_pio_clkdiv_int_frac8(2, 0);

    uint32_t country = CYW43_COUNTRY_WORLDWIDE;
    if (cyw_country >= 0)
        country = CYW43_COUNTRY(
            cyw_country_abbr[cyw_country][0],
            cyw_country_abbr[cyw_country][1],
            0);
    if (cyw43_arch_init_with_country(country))
        mon_add_response_str(STR_ERR_CYW_FAILED_TO_INIT);
    else
    {
        cyw_led_status = cyw_led_requested;
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, cyw_led_status);
    }
}

void cyw_load_rf_enable(const char *str, size_t len)
{
    str_parse_uint8(&str, &len, &cyw_rf_enable);
    if (cyw_rf_enable > 1)
        cyw_rf_enable = 0;
}

bool cyw_set_rf_enable(uint8_t rf)
{
    if (rf > 1)
        return false;
    if (cyw_rf_enable != rf)
    {
        cyw_rf_enable = rf;
        cyw_reset_radio();
        cfg_save();
    }
    return true;
}

uint8_t cyw_get_rf_enable(void)
{
    return cyw_rf_enable;
}

void cyw_load_rf_country_code(const char *str, size_t len)
{
    (void)len;
    cyw_country = cyw_lookup_country(str);
}

bool cyw_set_rf_country_code(const char *rfcc)
{
    int country = cyw_lookup_country(rfcc);
    if (*rfcc && country < 0)
        return false;
    if (cyw_country != country)
    {
        cyw_country = country;
        cyw_reset_radio();
        cfg_save();
    }
    return true;
}

const char *cyw_get_rf_country_code(void)
{
    if (cyw_country < 0)
        return "";
    else
        return cyw_country_abbr[cyw_country];
}

const char *cyw_get_rf_country_code_verbose(void)
{
    if (cyw_country < 0)
        return "";
    else
        return cyw_country_name[cyw_country];
}

int cyw_country_code_response(char *buf, size_t buf_size, int state)
{
    if (state < 0)
        return state;
    const char fmt[] = "  %2s - %-19s";
    unsigned rows = (CYW_COUNTRY_COUNT + 2) / 3;
    unsigned el = state;
    for (int i = 0; i < 3; i++)
    {
        if (el < CYW_COUNTRY_COUNT)
        {
            snprintf(buf, buf_size, fmt, cyw_country_abbr[el], cyw_country_name[el]);
            size_t n = strlen(buf);
            buf += n;
            buf_size -= n;
        }
        el += rows;
    }
    *buf++ = '\n';
    *buf = 0;
    return (unsigned)(state + 1) < rows ? state + 1 : -1;
}

#endif /* RP6502_RIA_W */
