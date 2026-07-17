/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "api/clk.h"
#include "api/oem.h"
#include "ble/ble.h"
#include "hid/kbd.h"
#include "mon/mon.h"
#include "mon/rom.h"
#include "mon/set.h"
#include "net/cyw.h"
#include "net/wfi.h"
#include "str/str.h"
#include "sys/com.h"
#include "sys/cfg.h"
#include "sys/com_hw.h"
#include "sys/cpu_hw.h"
#include "sys/vga.h"
#include "usb/nfc.h"
#include <stdio.h>
#include <pico.h>

#if defined(DEBUG_RIA_MON) || defined(DEBUG_RIA_MON_SET)
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

static int set_phi2_response(char *buf, size_t buf_size, int state, unsigned)
{
    (void)state;
    snprintf(buf, buf_size, STR_SET_PHI2_RESPONSE, cpu_get_phi2_khz());
    return -1;
}

static void set_phi2(const char *args)
{
    uint32_t val;
    if (*args && (!str_parse_uint32(&args, &val) ||
                  !str_parse_end(args) ||
                  !cpu_set_phi2_khz(val)))
        mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
    else
        mon_add_response_fn(set_phi2_response);
}

static int set_boot_response(char *buf, size_t buf_size, int state, unsigned)
{
    (void)state;
    const char *rom = rom_get_boot();
    if (!rom[0])
        rom = S(STR_PARENS_NONE);
    snprintf(buf, buf_size, STR_SET_BOOT_RESPONSE, rom);
    return -1;
}

static void set_boot(const char *args)
{
    if (*args)
    {
        const char *scan = args;
        const char *tok = str_parse_string(&scan);
        if (tok && !strcmp(tok, "-") && str_parse_end(scan) && *args != '"')
            rom_set_boot("");
        else
        {
            if (!rom_set_boot(args))
                return mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
        }
    }
    mon_add_response_fn(set_boot_response);
}

static int set_code_page_response(char *buf, size_t buf_size, int state, unsigned)
{
    (void)state;
    if (oem_is_auto())
        snprintf(buf, buf_size, STR_SET_CODE_PAGE_AUTO_RESPONSE, oem_get_code_page_run());
    else
        snprintf(buf, buf_size, STR_SET_CODE_PAGE_RESPONSE, oem_get_code_page());
    return -1;
}

static void set_code_page(const char *args)
{
    uint16_t val;
    if (*args && (!str_parse_uint16(&args, &val) ||
                  !str_parse_end(args) ||
                  !oem_set_code_page(val)))
        mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
    else
        mon_add_response_fn(set_code_page_response);
}

static int set_vga_response(char *buf, size_t buf_size, int state, unsigned)
{
    (void)state;
    snprintf(buf, buf_size, STR_SET_VGA_RESPONSE,
             vga_get_display_type(), vga_get_display_type_verbose());
    return -1;
}

static void set_vga(const char *args)
{
    uint32_t val;
    if (*args && (!str_parse_uint32(&args, &val) ||
                  !str_parse_end(args) ||
                  !vga_set_display_type(val)))
        mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
    else
        mon_add_response_fn(set_vga_response);
}

#ifdef RP6502_RIA_W

static int set_rf_response(char *buf, size_t buf_size, int state, unsigned)
{
    (void)state;
    uint8_t en = cyw_get_rf_enable();
    snprintf_utf8(buf, buf_size, STR_SET_RF_RESPONSE,
                  en, en ? S(STR_ON) : S(STR_OFF));
    return -1;
}

static void set_rf(const char *args)
{
    uint32_t val;
    if (*args && (!str_parse_uint32(&args, &val) ||
                  !str_parse_end(args) ||
                  !cyw_set_rf_enable(val)))
        mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
    else
        mon_add_response_fn(set_rf_response);
}

static int set_rfcc_response(char *buf, size_t buf_size, int state, unsigned)
{
    (void)state;
    const char *cc = cyw_get_rf_country_code();
    if (strlen(cc))
        snprintf_utf8(buf, buf_size, STR_SET_RFCC_RESPONSE,
                      cc, " ", cyw_get_rf_country_code_verbose());
    else
        snprintf_utf8(buf, buf_size, STR_SET_RFCC_RESPONSE,
                      "", "", S(STR_WORLDWIDE));
    return -1;
}

static void set_rfcc(const char *args)
{
    if (*args)
    {
        const char *scan = args;
        const char *tok = str_parse_string(&scan);
        if (tok && !strcmp(tok, "-") && str_parse_end(scan) && *args != '"')
            cyw_set_rf_country_code("");
        else
        {
            if (!tok || !str_parse_end(scan) || !cyw_set_rf_country_code(tok))
                return mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
        }
    }
    mon_add_response_fn(set_rfcc_response);
}

static int set_ssid_response(char *buf, size_t buf_size, int state, unsigned)
{
    (void)state;
    const char *ssid = wfi_get_ssid();
#if RP6502_CREATOR
    snprintf_utf8(buf, buf_size, STR_SET_SSID_RESPONSE,
                  strlen(ssid) ? S(STR_PARENS_SET) : S(STR_PARENS_NONE));
#else
    snprintf_utf8(buf, buf_size, STR_SET_SSID_RESPONSE,
                  strlen(ssid) ? ssid : S(STR_PARENS_NONE));
#endif
    return -1;
}

static int set_pass_response(char *buf, size_t buf_size, int state, unsigned)
{
    (void)state;
    const char *pass = wfi_get_pass();
    snprintf_utf8(buf, buf_size, STR_SET_PASS_RESPONSE,
                  strlen(pass) ? S(STR_PARENS_SET) : S(STR_PARENS_NONE));
    return -1;
}

static void set_ssid(const char *args)
{
    if (!*args)
        return mon_add_response_fn(set_ssid_response);
    const char *scan = args;
    const char *tok = str_parse_string(&scan);
    if (tok && !strcmp(tok, "-") && str_parse_end(scan) && *args != '"')
        wfi_set_ssid("");
    else
    {
        if (!tok || !str_parse_end(scan) || !wfi_set_ssid(tok))
            return mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
    }
    mon_add_response_fn(set_ssid_response);
    mon_add_response_fn(set_pass_response);
}

static void set_pass(const char *args)
{
    if (!*args)
        return mon_add_response_fn(set_pass_response);
    const char *scan = args;
    const char *tok = str_parse_string(&scan);
    if (tok && !strcmp(tok, "-") && str_parse_end(scan) && *args != '"')
        wfi_set_pass("");
    else
    {
        if (!tok || !str_parse_end(scan) || !wfi_set_pass(tok))
            return mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
    }
    mon_add_response_fn(set_ssid_response);
    mon_add_response_fn(set_pass_response);
}

static int set_ble_response(char *buf, size_t buf_size, int state, unsigned)
{
    (void)state;
    uint8_t en = ble_get_enabled();
    snprintf_utf8(buf, buf_size, STR_SET_BLE_RESPONSE,
                  en, en ? S(STR_ENABLED) : S(STR_DISABLED),
                  ble_is_pairing() ? S(STR_BLE_PAIRING) : "",
                  cyw_get_rf_enable() ? "" : S(STR_BLE_NO_RF));
    return -1;
}

static void set_ble(const char *args)
{
    uint32_t val;
    if (*args && (!str_parse_uint32(&args, &val) ||
                  !str_parse_end(args) ||
                  !ble_set_enabled(val)))
        mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
    else
        mon_add_response_fn(set_ble_response);
}

static int set_key_response(char *buf, size_t buf_size, int state, unsigned)
{
    (void)state;
    const char *key = com_tel_get_key();
    snprintf_utf8(buf, buf_size, STR_SET_KEY_RESPONSE,
                  strlen(key) ? S(STR_PARENS_SET) : S(STR_PARENS_NONE));
    return -1;
}

static int set_port_response(char *buf, size_t buf_size, int state, unsigned)
{
    (void)state;
    bool en = com_tel_get_port() > 0 && com_tel_get_key()[0];
    snprintf_utf8(buf, buf_size, STR_SET_PORT_RESPONSE,
                  com_tel_get_port(), en ? S(STR_ENABLED) : S(STR_DISABLED));
    return -1;
}

static void set_port(const char *args)
{
    if (!*args)
        return mon_add_response_fn(set_port_response);
    uint16_t val;
    if (!str_parse_uint16(&args, &val) ||
        !str_parse_end(args) ||
        !com_tel_set_port(val))
        return mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
    mon_add_response_fn(set_port_response);
    mon_add_response_fn(set_key_response);
}

static void set_key(const char *args)
{
    if (!*args)
        return mon_add_response_fn(set_key_response);
    const char *scan = args;
    const char *tok = str_parse_string(&scan);
    if (tok && !strcmp(tok, "-") && str_parse_end(scan) && *args != '"')
        com_tel_set_key("");
    else
    {
        if (!tok || !str_parse_end(scan) || !com_tel_set_key(tok))
            return mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
    }
    mon_add_response_fn(set_port_response);
    mon_add_response_fn(set_key_response);
}

#endif

static int set_nfc_response(char *buf, size_t buf_size, int state, unsigned)
{
    (void)state;
    uint8_t en = nfc_get_enabled();
    snprintf_utf8(buf, buf_size, STR_SET_NFC_RESPONSE,
                  en, en ? S(STR_ENABLED) : S(STR_DISABLED));
    return -1;
}

static void set_nfc(const char *args)
{
    uint32_t val;
    if (*args && (!str_parse_uint32(&args, &val) ||
                  !str_parse_end(args) ||
                  !nfc_set_enabled(val)))
        mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
    else
        mon_add_response_fn(set_nfc_response);
}

static int set_time_zone_response(char *buf, size_t buf_size, int state, unsigned)
{
    (void)state;
    snprintf(buf, buf_size, STR_SET_TZ_RESPONSE, clk_get_time_zone());
    return -1;
}

static void set_time_zone(const char *args)
{
    if (*args)
    {
        const char *tok = str_parse_string(&args);
        if (!tok || !str_parse_end(args) || !clk_set_time_zone(tok))
        {
            mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
            return;
        }
    }
    mon_add_response_fn(set_time_zone_response);
}

static int set_locale_response(char *buf, size_t buf_size, int state, unsigned)
{
    (void)state;
    snprintf(buf, buf_size, STR_SET_LOC_RESPONSE,
             str_get_locale(), str_get_locale_verbose());
    return -1;
}

static void set_locale(const char *args)
{
    if (*args)
    {
        const char *tok = str_parse_string(&args);
        if (!tok || !str_parse_end(args) || !str_set_locale(tok))
        {
            mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
            return;
        }
    }
    mon_add_response_fn(set_locale_response);
}

static int set_kbd_layout_response(char *buf, size_t buf_size, int state, unsigned)
{
    (void)state;
    const char *list = kbd_get_layout_list();
    if (strchr(list, ' '))
        snprintf(buf, buf_size, STR_SET_KB_LIST_RESPONSE, list);
    else
        snprintf(buf, buf_size, STR_SET_KB_RESPONSE, kbd_get_layout(), kbd_get_layout_verbose());
    return -1;
}

static void set_kbd_layout(const char *args)
{
    if (*args && !kbd_set_layout(args))
    {
        mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
        return;
    }
    mon_add_response_fn(set_kbd_layout_response);
}

typedef void (*set_function)(const char *);
__in_flash("set_attributes") static struct
{
    const char *const attr;
    set_function func;
} const SET_ATTRIBUTES[] = {
    {STR_PHI2, set_phi2},
    {STR_BOOT, set_boot},
    {STR_TZ, set_time_zone},
    {STR_LOC, set_locale},
    {STR_KB, set_kbd_layout},
    {STR_CP, set_code_page},
    {STR_VGA, set_vga},
    {STR_NFC, set_nfc},
#ifdef RP6502_RIA_W
    {STR_RF, set_rf},
    {STR_RFCC, set_rfcc},
    {STR_SSID, set_ssid},
    {STR_PASS, set_pass},
    {STR_PORT, set_port},
    {STR_KEY, set_key},
    {STR_BLE, set_ble},
#endif
};
static const size_t SET_ATTRIBUTES_COUNT = sizeof SET_ATTRIBUTES / sizeof *SET_ATTRIBUTES;

void set_mon_set(const char *args)
{
    if (*args)
    {
        const char *attr = str_parse_string(&args);
        if (attr)
        {
            for (size_t i = 0; i < SET_ATTRIBUTES_COUNT; i++)
            {
                if (!strcasecmp(attr, SET_ATTRIBUTES[i].attr))
                {
                    SET_ATTRIBUTES[i].func(args);
                    return;
                }
            }
        }
        mon_add_response_utf8(S(STR_ERR_INVALID_ARGUMENT));
        return;
    }
    // No args, show everything
    mon_add_response_fn(set_phi2_response);
    mon_add_response_fn(set_boot_response);
    mon_add_response_fn(set_time_zone_response);
    mon_add_response_fn(set_locale_response);
    mon_add_response_fn(set_kbd_layout_response);
    mon_add_response_fn(set_code_page_response);
    mon_add_response_fn(set_vga_response);
    mon_add_response_fn(set_nfc_response);
#ifdef RP6502_RIA_W
    mon_add_response_fn(set_rf_response);
    mon_add_response_fn(set_rfcc_response);
    mon_add_response_fn(set_ssid_response);
    mon_add_response_fn(set_pass_response);
    mon_add_response_fn(set_port_response);
    mon_add_response_fn(set_key_response);
    mon_add_response_fn(set_ble_response);
#endif
}
