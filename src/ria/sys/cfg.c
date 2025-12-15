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
#include "net/ble.h"
#include "net/cyw.h"
#include "net/wfi.h"
#include "str/str.h"
#include "sys/cfg.h"
#include "sys/cpu.h"
#include "sys/lfs.h"
#include "sys/mem.h"
#include "sys/vga.h"
#include <ctype.h>

#if defined(DEBUG_RIA_SYS) || defined(DEBUG_RIA_SYS_CFG)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// Configuration is a plain ASCII file on the LFS. e.g.
// +V1         | Version - Must be first
// +P8000      | PHI2
// +C0         | Caps (retired)
// +R0         | RESB (retired)
// +TUTC0      | Time Zone
// +S437       | Code Page
// +LUS        | Keyboard Layout
// +D0         | VGA display type
// +E1         | RF Enabled
// +FUS        | RF Country Code
// +WMyWiFi    | WiFi SSID
// +KsEkRiT    | WiFi Password
// +B1         | Bluetooth Enabled
// BASIC       | Boot ROM - Must be last

#define CFG_VERSION 1

static uint8_t cfg_vga_display;

#ifdef RP6502_RIA_W
static uint8_t cfg_net_rf = 1;
static char cfg_net_rfcc[3];
static char cfg_net_ssid[33];
static char cfg_net_pass[65];
static uint8_t cfg_net_ble = 1;
#endif /* RP6502_RIA_W */

// Optional string can replace boot string
static void cfg_save_with_boot_opt(const char *opt_str)
{
    lfs_file_t lfs_file;
    LFS_FILE_CONFIG(lfs_file_config);
    int lfsresult = lfs_file_opencfg(&lfs_volume, &lfs_file, STR_CFG_FILENAME,
                                     LFS_O_RDWR | LFS_O_CREAT,
                                     &lfs_file_config);
    if (lfsresult < 0)
    {
        printf("?Unable to lfs_file_opencfg %s for writing (%d)\n", STR_CFG_FILENAME, lfsresult);
        return;
    }
    if (!opt_str)
    {
        opt_str = (char *)mbuf;
        // Fetch the boot string, ignore the rest
        while (lfs_gets((char *)mbuf, MBUF_SIZE, &lfs_volume, &lfs_file))
            if (mbuf[0] != '+')
                break;
        if (lfsresult >= 0)
            if ((lfsresult = lfs_file_rewind(&lfs_volume, &lfs_file)) < 0)
                printf("?Unable to lfs_file_rewind %s (%d)\n", STR_CFG_FILENAME, lfsresult);
    }

    if (lfsresult >= 0)
        if ((lfsresult = lfs_file_truncate(&lfs_volume, &lfs_file, 0)) < 0)
            printf("?Unable to lfs_file_truncate %s (%d)\n", STR_CFG_FILENAME, lfsresult);
    if (lfsresult >= 0)
    {
        lfsresult = lfs_printf(&lfs_volume, &lfs_file,
                               "+V%u\n"
                               "+P%u\n"
                               "+T%s\n"
                               "+S%u\n"
                               "+L%s\n"
                               "+D%u\n"
#ifdef RP6502_RIA_W
                               "+E%u\n"
                               "+F%s\n"
                               "+W%s\n"
                               "+K%s\n"
                               "+B%u\n"
#endif /* RP6502_RIA_W */
                               "%s",
                               CFG_VERSION,
                               cpu_get_phi2_khz(),
                               clk_get_time_zone(),
                               oem_get_code_page(),
                               kbd_get_layout(),
                               cfg_vga_display,
#ifdef RP6502_RIA_W
                               cfg_net_rf,
                               cfg_net_rfcc,
                               cfg_net_ssid,
                               cfg_net_pass,
                               cfg_net_ble,
#endif /* RP6502_RIA_W */
                               opt_str);
        if (lfsresult < 0)
            printf("?Unable to write %s contents (%d)\n", STR_CFG_FILENAME, lfsresult);
    }
    int lfscloseresult = lfs_file_close(&lfs_volume, &lfs_file);
    if (lfscloseresult < 0)
        printf("?Unable to lfs_file_close %s (%d)\n", STR_CFG_FILENAME, lfscloseresult);
    if (lfsresult < 0 || lfscloseresult < 0)
        lfs_remove(&lfs_volume, STR_CFG_FILENAME);
}

static void cfg_load_with_boot_opt(bool boot_only)
{
    lfs_file_t lfs_file;
    LFS_FILE_CONFIG(lfs_file_config);
    int lfsresult = lfs_file_opencfg(&lfs_volume, &lfs_file, STR_CFG_FILENAME,
                                     LFS_O_RDONLY, &lfs_file_config);
    mbuf[0] = 0;
    if (lfsresult < 0)
    {
        if (lfsresult != LFS_ERR_NOENT)
            printf("?Unable to lfs_file_opencfg %s for reading (%d)\n", STR_CFG_FILENAME, lfsresult);
        return;
    }
    while (lfs_gets((char *)mbuf, MBUF_SIZE, &lfs_volume, &lfs_file))
    {
        if (mbuf[0] != '+')
            break;
        if (boot_only)
            continue;
        size_t len = strlen((char *)mbuf);
        while (len && mbuf[len - 1] == '\n')
            len--;
        mbuf[len] = 0;
        const char *str = (char *)mbuf + 2;
        len -= 2;
        switch (mbuf[1])
        {
        case 'P':
            cpu_load_phi2_khz(str, len);
            break;
        case 'T':
            clk_load_time_zone(str, len);
            break;
        case 'S':
            oem_load_code_page(str, len);
            break;
        case 'L':
            kbd_load_layout(str, len);
            break;
        case 'D':
            str_parse_uint8(&str, &len, &cfg_vga_display);
            break;
#ifdef RP6502_RIA_W
        case 'E':
            str_parse_uint8(&str, &len, &cfg_net_rf);
            break;
        case 'F':
            str_parse_string(&str, &len, cfg_net_rfcc, sizeof(cfg_net_rfcc));
            break;
        case 'W':
            str_parse_string(&str, &len, cfg_net_ssid, sizeof(cfg_net_ssid));
            break;
        case 'K':
            str_parse_string(&str, &len, cfg_net_pass, sizeof(cfg_net_pass));
            break;
        case 'B':
            str_parse_uint8(&str, &len, &cfg_net_ble);
            break;
#endif /* RP6502_RIA_W */
        default:
            break;
        }
    }
    lfsresult = lfs_file_close(&lfs_volume, &lfs_file);
    if (lfsresult < 0)
        printf("?Unable to lfs_file_close %s (%d)\n", STR_CFG_FILENAME, lfsresult);
}

void cfg_init(void)
{
    cfg_load_with_boot_opt(false);
}

void cfg_save(void)
{
    cfg_save_with_boot_opt(NULL);
}

void cfg_save_boot(const char *str)
{
    cfg_save_with_boot_opt(str);
}

const char *cfg_load_boot(void)
{
    cfg_load_with_boot_opt(true);
    return (char *)mbuf;
}

bool cfg_set_vga(uint8_t disp)
{
    if (!vga_set_vga(disp))
        return false;
    if (cfg_vga_display != disp)
    {
        cfg_vga_display = disp;
        cfg_save_with_boot_opt(NULL);
    }
    return true;
}

uint8_t cfg_get_vga(void)
{
    return cfg_vga_display;
}

#ifdef RP6502_RIA_W

bool cfg_set_rf(uint8_t rf)
{
    if (rf > 1)
        return false;
    if (cfg_net_rf != rf)
    {
        cfg_net_rf = rf;
        cyw_reset_radio();
        cfg_save_with_boot_opt(NULL);
    }
    return true;
}

uint8_t cfg_get_rf(void)
{
    return cfg_net_rf;
}

bool cfg_set_rfcc(const char *rfcc)
{
    char cc[3] = {0, 0, 0};
    size_t len = strlen(rfcc);
    if (len == 2)
    {
        cc[0] = toupper(rfcc[0]);
        cc[1] = toupper(rfcc[1]);
        if (!cyw_validate_country_code(cc))
            return false;
    }
    if (len == 0 || len == 2)
    {
        if (strcmp(cfg_net_rfcc, cc))
        {
            strcpy(cfg_net_rfcc, cc);
            cyw_reset_radio();
            cfg_save_with_boot_opt(NULL);
        }
        return true;
    }
    return false;
}

const char *cfg_get_rfcc(void)
{
    return cfg_net_rfcc;
}

bool cfg_set_ssid(const char *ssid)
{
    size_t len = strlen(ssid);
    if (len < sizeof(cfg_net_ssid) - 1)
    {
        if (strcmp(cfg_net_ssid, ssid))
        {
            cfg_net_pass[0] = 0;
            strncpy(cfg_net_ssid, ssid, sizeof(cfg_net_ssid));
            wfi_shutdown();
            cfg_save_with_boot_opt(NULL);
        }
        return true;
    }
    return false;
}

const char *cfg_get_ssid(void)
{
    return cfg_net_ssid;
}

bool cfg_set_pass(const char *pass)
{
    if (strlen(cfg_net_ssid) && strlen(pass) < sizeof(cfg_net_pass) - 1)
    {
        if (strcmp(cfg_net_pass, pass))
        {
            strcpy(cfg_net_pass, pass);
            wfi_shutdown();
            cfg_save_with_boot_opt(NULL);
        }
        return true;
    }
    return false;
}

const char *cfg_get_pass(void)
{
    return cfg_net_pass;
}

bool cfg_set_ble(uint8_t ble)
{
    if (ble > 2)
        return false;
    ble_set_config(ble);
    if (ble == 2)
        ble = 1;
    if (cfg_net_ble != ble)
    {
        cfg_net_ble = ble;
        cfg_save_with_boot_opt(NULL);
    }
    return true;
}

uint8_t cfg_get_ble(void)
{
    return cfg_net_ble;
}

#endif /* RP6502_RIA_W */
