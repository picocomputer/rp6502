/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "hid/kbd.h"
#include "mon/mon.h"
#include "mon/set.h"
#include "net/ble.h"
#include "str/str.h"
#include "sys/cfg.h"
#include "sys/cpu.h"
#include "sys/lfs.h"

#if defined(DEBUG_RIA_MON) || defined(DEBUG_RIA_MON_SET)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
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
    const char *rom = cfg_get_boot();
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
            cfg_set_boot("");
        else if (!str_parse_rom_name(&args, &len, lfs_name) ||
                 !str_parse_end(args, len) ||
                 !cfg_set_boot(lfs_name))
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
    snprintf(buf, buf_size, STR_SET_CODE_PAGE_RESPONSE, cfg_get_code_page());
#endif
    return -1;
}

static void set_code_page(const char *args, size_t len)
{
    uint32_t val;
    if (len && (!str_parse_uint32(&args, &len, &val) ||
                !str_parse_end(args, len) ||
                !cfg_set_code_page(val)))
        mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
    else
        mon_add_response_fn(set_code_page_response);
}

static int set_vga_response(char *buf, size_t buf_size, int state)
{
    (void)state;
    const char *const vga_labels[] = {STR_SET_VGA_0_LABEL, STR_SET_VGA_1_LABEL, STR_SET_VGA_2_LABEL};
    snprintf(buf, buf_size, STR_SET_VGA_RESPONSE, vga_labels[cfg_get_vga()]);
    return -1;
}

static void set_vga(const char *args, size_t len)
{
    uint32_t val;
    if (len && (!str_parse_uint32(&args, &len, &val) ||
                !str_parse_end(args, len)))
        mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
    else
        cfg_set_vga(val);
    mon_add_response_fn(set_vga_response);
}

#ifdef RP6502_RIA_W

static int set_rf_response(char *buf, size_t buf_size, int state)
{
    (void)state;
    snprintf(buf, buf_size, STR_SET_RF_RESPONSE,
             cfg_get_rf() ? STR_ON : STR_OFF);
    return -1;
}

static void set_rf(const char *args, size_t len)
{
    uint32_t val;
    if (len && (!str_parse_uint32(&args, &len, &val) ||
                !str_parse_end(args, len) ||
                !cfg_set_rf(val)))
        mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
    else
        mon_add_response_fn(set_rf_response);
}

static int set_rfcc_response(char *buf, size_t buf_size, int state)
{
    (void)state;
    const char *cc = cfg_get_rfcc();
    snprintf(buf, buf_size, STR_SET_RFCC_RESPONSE,
             strlen(cc) ? cc : STR_WORLDWIDE);
    return -1;
}

static void set_rfcc(const char *args, size_t len)
{
    char rfcc[3];
    if (len)
    {
        if (args[0] == '-' && str_parse_end(++args, --len))
            cfg_set_rfcc("");
        else if (!str_parse_string(&args, &len, rfcc, sizeof(rfcc)) ||
                 !str_parse_end(args, len) ||
                 !cfg_set_rfcc(rfcc))
            return mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
    }
    mon_add_response_fn(set_rfcc_response);
}

static int set_ssid_response(char *buf, size_t buf_size, int state)
{
    (void)state;
    const char *cc = cfg_get_ssid();
    snprintf(buf, buf_size, STR_SET_SSID_RESPONSE,
             strlen(cc) ? cc : STR_PARENS_NONE);
    return -1;
}

static int set_pass_response(char *buf, size_t buf_size, int state)
{
    (void)state;
    const char *pass = cfg_get_pass();
    snprintf(buf, buf_size, STR_SET_PASS_RESPONSE,
             strlen(pass) ? STR_PARENS_SET : STR_PARENS_NONE);
    return -1;
}

static void set_ssid(const char *args, size_t len)
{
    char ssid[33];
    if (!len)
        return mon_add_response_fn(set_ssid_response);
    if (args[0] == '-' && str_parse_end(++args, --len))
        cfg_set_ssid("");
    else if (!str_parse_string(&args, &len, ssid, sizeof(ssid)) ||
             !str_parse_end(args, len) ||
             !cfg_set_ssid(ssid))
        return mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
    mon_add_response_fn(set_ssid_response);
    mon_add_response_fn(set_pass_response);
}

static void set_pass(const char *args, size_t len)
{
    char pass[65];
    if (!len)
        return mon_add_response_fn(set_pass_response);
    if (args[0] == '-' && str_parse_end(++args, --len))
        cfg_set_pass("");
    else if (!str_parse_string(&args, &len, pass, sizeof(pass)) ||
             !str_parse_end(args, len) ||
             !cfg_set_pass(pass))
        return mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
    mon_add_response_fn(set_ssid_response);
    mon_add_response_fn(set_pass_response);
}

static int set_ble_response(char *buf, size_t buf_size, int state)
{
    (void)state;
    snprintf(buf, buf_size, STR_SET_BLE_RESPONSE,
             cfg_get_ble() ? STR_ENABLED : STR_DISABLED,
             ble_is_pairing() ? STR_BLE_PAIRING : "",
             cfg_get_rf() ? "" : STR_BLE_NO_RF);
    return -1;
}

static void set_ble(const char *args, size_t len)
{
    uint32_t val;
    if (len && (!str_parse_uint32(&args, &len, &val) ||
                !str_parse_end(args, len) ||
                !cfg_set_ble(val)))
        mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
    else
        mon_add_response_fn(set_ble_response);
}

#endif

static int set_time_zone_response(char *buf, size_t buf_size, int state)
{
    (void)state;
    snprintf(buf, buf_size, "TZ  : %s\n", cfg_get_time_zone());
    return -1;
}

static void set_time_zone(const char *args, size_t len)
{
    char tz[65];
    if (len && (!str_parse_string(&args, &len, tz, sizeof(tz)) ||
                !str_parse_end(args, len) ||
                !cfg_set_time_zone(tz)))
        mon_add_response_str(STR_ERR_INVALID_ARGUMENT);
    else
        mon_add_response_fn(set_time_zone_response);
}

static int set_kbd_layout_response(char *buf, size_t buf_size, int state)
{
    (void)state;
    snprintf(buf, buf_size, STR_SET_KB_RESPONSE, cfg_get_kbd_layout());
    return -1;
}

static void set_kbd_layout(const char *args, size_t len)
{
    char kb[KBD_LAYOUT_MAX_NAME_SIZE];
    if (len && (!str_parse_string(&args, &len, kb, sizeof(kb)) ||
                !str_parse_end(args, len) ||
                !cfg_set_kbd_layout(kb)))
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

static int set_print_all_response(char *buf, size_t buf_size, int state)
{
    switch (state)
    {
    case 0:
        set_phi2_response(buf, buf_size, state);
        break;
    case 1:
        set_boot_response(buf, buf_size, state);
        break;
    case 2:
        set_time_zone_response(buf, buf_size, state);
        break;
    case 3:
        set_kbd_layout_response(buf, buf_size, state);
        break;
    case 4:
        set_code_page_response(buf, buf_size, state);
        break;
    case 5:
        set_vga_response(buf, buf_size, state);
        break;
#ifdef RP6502_RIA_W
    case 6:
        set_rf_response(buf, buf_size, state);
        break;
    case 7:
        set_rfcc_response(buf, buf_size, state);
        break;
    case 8:
        set_ssid_response(buf, buf_size, state);
        break;
    case 9:
        set_pass_response(buf, buf_size, state);
        break;
    case 10:
        set_ble_response(buf, buf_size, state);
        break;
#endif
    default:
        return -1;
    }
    return ++state;
}

void set_mon_set(const char *args, size_t len)
{
    if (!len)
        return mon_add_response_fn(set_print_all_response);
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
}
