/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "api/clk.h"
#include "api/oem.h"
#include "hid/kbd.h"
#include "mon/mon.h"
#include "mon/rom.h"
#include "mon/set.h"
#include "net/ble.h"
#include "net/cyw.h"
#include "net/wfi.h"
#include "str/str.h"
#include "sys/cfg.h"
#include "sys/cpu.h"
#include "sys/lfs.h"
#include "sys/vga.h"

#if defined(DEBUG_RIA_MON) || defined(DEBUG_RIA_MON_SET)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

static int set_phi2_response(char *buf, size_t buf_size, int state)
{
    (void)state;
    snprintf(buf, buf_size, STR_SET_PHI2_RESPONSE, cpu_get_phi2_khz());
    return -1;
}

static void set_phi2(const char *args, size_t len)
{
    uint32_t val;
    if (len && (!str_parse_uint32(&args, &len, &val) ||
                !str_parse_end(args, len) ||
                !cpu_set_phi2_khz(val)))
        mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
    else
        mon_add_response_fn(set_phi2_response);
}

static int set_boot_response(char *buf, size_t buf_size, int state)
{
    (void)state;
    const char *rom = rom_get_boot();
    if (!rom[0])
        rom = STR_PARENS_NONE;
    snprintf(buf, buf_size, STR_SET_BOOT_RESPONSE, rom);
    return -1;
}

static void set_boot(const char *args, size_t len)
{
    char lfs_name[LFS_NAME_MAX + 1];
    if (len)
    {
        if (args[0] == '-' && str_parse_end(++args, --len))
            rom_set_boot("");
        else if (!str_parse_rom_name(&args, &len, lfs_name) ||
                 !str_parse_end(args, len) ||
                 !rom_set_boot(lfs_name))
            return mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
    }
    mon_add_response_fn(set_boot_response);
}

static int set_code_page_response(char *buf, size_t buf_size, int state)
{
    (void)state;
#if (RP6502_CODE_PAGE)
    snprintf(buf, buf_size, STR_SET_CODE_PAGE_DEV_RESPONSE, RP6502_CODE_PAGE);
#else
    snprintf(buf, buf_size, STR_SET_CODE_PAGE_RESPONSE, oem_get_code_page());
#endif
    return -1;
}

static void set_code_page(const char *args, size_t len)
{
    uint32_t val;
    if (len && (!str_parse_uint32(&args, &len, &val) ||
                !str_parse_end(args, len) ||
                !oem_set_code_page(val)))
        mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
    else
        mon_add_response_fn(set_code_page_response);
}

static int set_vga_response(char *buf, size_t buf_size, int state)
{
    (void)state;
    snprintf(buf, buf_size, STR_SET_VGA_RESPONSE,
             vga_get_display_type(), vga_get_display_type_verbose());
    return -1;
}

static void set_vga(const char *args, size_t len)
{
    uint32_t val;
    if (len && (!str_parse_uint32(&args, &len, &val) ||
                !str_parse_end(args, len) ||
                !vga_set_display_type(val)))
        mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
    else
        mon_add_response_fn(set_vga_response);
}

#ifdef RP6502_RIA_W

static int set_rf_response(char *buf, size_t buf_size, int state)
{
    (void)state;
    uint8_t en = cyw_get_rf_enable();
    snprintf(buf, buf_size, STR_SET_RF_RESPONSE,
             en, en ? STR_ON : STR_OFF);
    return -1;
}

static void set_rf(const char *args, size_t len)
{
    uint32_t val;
    if (len && (!str_parse_uint32(&args, &len, &val) ||
                !str_parse_end(args, len) ||
                !cyw_set_rf_enable(val)))
        mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
    else
        mon_add_response_fn(set_rf_response);
}

static int set_rfcc_response(char *buf, size_t buf_size, int state)
{
    (void)state;
    const char *cc = cyw_get_rf_country_code();
    if (strlen(cc))
        snprintf(buf, buf_size, STR_SET_RFCC_RESPONSE,
                 cc, " ", cyw_get_rf_country_code_verbose());
    else
        snprintf(buf, buf_size, STR_SET_RFCC_RESPONSE,
                 "", "", STR_WORLDWIDE);
    return -1;
}

static void set_rfcc(const char *args, size_t len)
{
    char rfcc[3];
    if (len)
    {
        if (args[0] == '-' && str_parse_end(++args, --len))
            cyw_set_rf_country_code("");
        else if (!str_parse_string(&args, &len, rfcc, sizeof(rfcc)) ||
                 !str_parse_end(args, len) ||
                 !cyw_set_rf_country_code(rfcc))
            return mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
    }
    mon_add_response_fn(set_rfcc_response);
}

static int set_ssid_response(char *buf, size_t buf_size, int state)
{
    (void)state;
    const char *cc = wfi_get_ssid();
    snprintf(buf, buf_size, STR_SET_SSID_RESPONSE,
             strlen(cc) ? cc : STR_PARENS_NONE);
    return -1;
}

static int set_pass_response(char *buf, size_t buf_size, int state)
{
    (void)state;
    const char *pass = wfi_get_pass();
    snprintf(buf, buf_size, STR_SET_PASS_RESPONSE,
             strlen(pass) ? STR_PARENS_SET : STR_PARENS_NONE);
    return -1;
}

static void set_ssid(const char *args, size_t len)
{
    char ssid[WFI_SSID_SIZE];
    if (!len)
        return mon_add_response_fn(set_ssid_response);
    if (args[0] == '-' && str_parse_end(++args, --len))
        wfi_set_ssid("");
    else if (!str_parse_string(&args, &len, ssid, sizeof(ssid)) ||
             !str_parse_end(args, len) ||
             !wfi_set_ssid(ssid))
        return mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
    mon_add_response_fn(set_ssid_response);
    mon_add_response_fn(set_pass_response);
}

static void set_pass(const char *args, size_t len)
{
    char pass[WFI_PASS_SIZE];
    if (!len)
        return mon_add_response_fn(set_pass_response);
    if (args[0] == '-' && str_parse_end(++args, --len))
        wfi_set_pass("");
    else if (!str_parse_string(&args, &len, pass, sizeof(pass)) ||
             !str_parse_end(args, len) ||
             !wfi_set_pass(pass))
        return mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
    mon_add_response_fn(set_ssid_response);
    mon_add_response_fn(set_pass_response);
}

static int set_ble_response(char *buf, size_t buf_size, int state)
{
    (void)state;
    uint8_t en = ble_get_enabled();
    snprintf(buf, buf_size, STR_SET_BLE_RESPONSE,
             en, en ? STR_ENABLED : STR_DISABLED,
             ble_is_pairing() ? STR_BLE_PAIRING : "",
             cyw_get_rf_enable() ? "" : STR_BLE_NO_RF);
    return -1;
}

static void set_ble(const char *args, size_t len)
{
    uint32_t val;
    if (len && (!str_parse_uint32(&args, &len, &val) ||
                !str_parse_end(args, len) ||
                !ble_set_enabled(val)))
        mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
    else
        mon_add_response_fn(set_ble_response);
}

#endif

static int set_time_zone_response(char *buf, size_t buf_size, int state)
{
    (void)state;
    snprintf(buf, buf_size, STR_SET_TZ_RESPONSE, clk_get_time_zone());
    return -1;
}

static void set_time_zone(const char *args, size_t len)
{
    char tz[CLK_TZ_MAX_SIZE];
    if (len && (!str_parse_string(&args, &len, tz, sizeof(tz)) ||
                !str_parse_end(args, len) ||
                !clk_set_time_zone(tz)))
        mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
    else
        mon_add_response_fn(set_time_zone_response);
}

static int set_kbd_layout_response(char *buf, size_t buf_size, int state)
{
    (void)state;
    snprintf(buf, buf_size, STR_SET_KB_RESPONSE, kbd_get_layout(), kbd_get_layout_verbose());
    return -1;
}

static void set_kbd_layout(const char *args, size_t len)
{
    char kb[KBD_LAYOUT_MAX_NAME_SIZE];
    if (len && (!str_parse_string(&args, &len, kb, sizeof(kb)) ||
                !str_parse_end(args, len) ||
                !kbd_set_layout(kb)))
        mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
    else
        mon_add_response_fn(set_kbd_layout_response);
}

typedef void (*set_function)(const char *, size_t);
__in_flash("set_attributes") static struct
{
    const char *const attr;
    set_function func;
} const SET_ATTRIBUTES[] = {
    {STR_PHI2, set_phi2},
    {STR_BOOT, set_boot},
    {STR_TZ, set_time_zone},
    {STR_KB, set_kbd_layout},
    {STR_CP, set_code_page},
    {STR_VGA, set_vga},
#ifdef RP6502_RIA_W
    {STR_RF, set_rf},
    {STR_RFCC, set_rfcc},
    {STR_SSID, set_ssid},
    {STR_PASS, set_pass},
    {STR_BLE, set_ble},
#endif
};
static const size_t SET_ATTRIBUTES_COUNT = sizeof SET_ATTRIBUTES / sizeof *SET_ATTRIBUTES;

void set_mon_set(const char *args, size_t len)
{
    if (len)
    {
        size_t i = 0;
        for (; i < len; i++)
            if (args[i] == ' ')
                break;
        size_t attr_len = i;
        for (; i < len; i++)
            if (args[i] != ' ')
                break;
        size_t args_start = i;
        for (i = 0; i < SET_ATTRIBUTES_COUNT; i++)
        {
            if (attr_len == strlen(SET_ATTRIBUTES[i].attr) &&
                !strncasecmp(args, SET_ATTRIBUTES[i].attr, attr_len))
            {
                SET_ATTRIBUTES[i].func(&args[args_start], len - args_start);
                return;
            }
        }
        mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
        return;
    }
    // No args, show everything
    mon_add_response_fn(set_phi2_response);
    mon_add_response_fn(set_boot_response);
    mon_add_response_fn(set_time_zone_response);
    mon_add_response_fn(set_kbd_layout_response);
    mon_add_response_fn(set_code_page_response);
    mon_add_response_fn(set_vga_response);
#ifdef RP6502_RIA_W
    mon_add_response_fn(set_rf_response);
    mon_add_response_fn(set_rfcc_response);
    mon_add_response_fn(set_ssid_response);
    mon_add_response_fn(set_pass_response);
    mon_add_response_fn(set_ble_response);
#endif
}
