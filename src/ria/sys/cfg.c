/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "str.h"
#include "api/clk.h"
#include "api/oem.h"
#include "net/cyw.h"
#include "net/wfi.h"
#include "sys/cfg.h"
#include "sys/cpu.h"
#include "sys/lfs.h"
#include "sys/mem.h"
#include "sys/vga.h"
#include <ctype.h>

// Configuration is a plain ASCII file on the LFS. e.g.
// +V1         | Version - Must be first
// +P8000      | PHI2
// +C0         | Caps
// +R0         | RESB
// +TUTC0      | Time Zone
// +S437       | Code Page
// +D0         | VGA display type
// +E1         | RF Enabled
// +FUS        | RF Country Code
// +WMyWiFi    | WiFi SSID
// +KsEkRiT    | WiFi Password
// BASIC       | Boot ROM - Must be last

#define CFG_VERSION 1
static const char filename[] = "CONFIG.SYS";

static uint32_t cfg_phi2_khz;
static uint8_t cfg_reset_ms;
static uint8_t cfg_caps;
static uint32_t cfg_codepage;
static uint8_t cfg_vga_display;
static char cfg_time_zone[65];

#ifdef RP6502_RIA_W
static uint8_t cfg_net_rf = 1;
static char cfg_net_rfcc[3];
static char cfg_net_ssid[33];
static char cfg_net_pass[65];
#endif /* RP6502_RIA_W */

// Optional string can replace boot string
static void cfg_save_with_boot_opt(char *opt_str)
{
    lfs_file_t lfs_file;
    LFS_FILE_CONFIG(lfs_file_config);
    int lfsresult = lfs_file_opencfg(&lfs_volume, &lfs_file, filename,
                                     LFS_O_RDWR | LFS_O_CREAT,
                                     &lfs_file_config);
    if (lfsresult < 0)
    {
        printf("?Unable to lfs_file_opencfg %s for writing (%d)\n", filename, lfsresult);
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
                printf("?Unable to lfs_file_rewind %s (%d)\n", filename, lfsresult);
    }

    if (lfsresult >= 0)
        if ((lfsresult = lfs_file_truncate(&lfs_volume, &lfs_file, 0)) < 0)
            printf("?Unable to lfs_file_truncate %s (%d)\n", filename, lfsresult);
    if (lfsresult >= 0)
    {
        lfsresult = lfs_printf(&lfs_volume, &lfs_file,
                               "+V%d\n"
                               "+P%d\n"
                               "+R%d\n"
                               "+C%d\n"
                               "+T%s\n"
                               "+S%d\n"
                               "+D%d\n"
#ifdef RP6502_RIA_W
                               "+E%d\n"
                               "+F%s\n"
                               "+W%s\n"
                               "+K%s\n"
#endif /* RP6502_RIA_W */
                               "%s",
                               CFG_VERSION,
                               cfg_phi2_khz,
                               cfg_reset_ms,
                               cfg_caps,
                               cfg_time_zone,
                               cfg_codepage,
                               cfg_vga_display,
#ifdef RP6502_RIA_W
                               cfg_net_rf,
                               cfg_net_rfcc,
                               cfg_net_ssid,
                               cfg_net_pass,
#endif /* RP6502_RIA_W */
                               opt_str);
        if (lfsresult < 0)
            printf("?Unable to write %s contents (%d)\n", filename, lfsresult);
    }
    int lfscloseresult = lfs_file_close(&lfs_volume, &lfs_file);
    if (lfscloseresult < 0)
        printf("?Unable to lfs_file_close %s (%d)\n", filename, lfscloseresult);
    if (lfsresult < 0 || lfscloseresult < 0)
        lfs_remove(&lfs_volume, filename);
}

static void cfg_load_with_boot_opt(bool boot_only)
{
    lfs_file_t lfs_file;
    LFS_FILE_CONFIG(lfs_file_config);
    int lfsresult = lfs_file_opencfg(&lfs_volume, &lfs_file, filename,
                                     LFS_O_RDONLY, &lfs_file_config);
    mbuf[0] = 0;
    if (lfsresult < 0)
    {
        if (lfsresult != LFS_ERR_NOENT)
            printf("?Unable to lfs_file_opencfg %s for reading (%d)\n", filename, lfsresult);
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
            parse_uint32(&str, &len, &cfg_phi2_khz);
            break;
        case 'R':
            parse_uint8(&str, &len, &cfg_reset_ms);
            break;
        case 'C':
            parse_uint8(&str, &len, &cfg_caps);
            break;
        case 'T':
            parse_string(&str, &len, cfg_time_zone, sizeof(cfg_time_zone));
            break;
        case 'S':
            parse_uint32(&str, &len, &cfg_codepage);
            break;
        case 'D':
            parse_uint8(&str, &len, &cfg_vga_display);
            break;
#ifdef RP6502_RIA_W
        case 'E':
            parse_uint8(&str, &len, &cfg_net_rf);
            break;
        case 'F':
            parse_string(&str, &len, cfg_net_rfcc, sizeof(cfg_net_rfcc));
            break;
        case 'W':
            parse_string(&str, &len, cfg_net_ssid, sizeof(cfg_net_ssid));
            break;
        case 'K':
            parse_string(&str, &len, cfg_net_pass, sizeof(cfg_net_pass));
            break;
#endif /* RP6502_RIA_W */
        default:
            break;
        }
    }
    lfsresult = lfs_file_close(&lfs_volume, &lfs_file);
    if (lfsresult < 0)
        printf("?Unable to lfs_file_close %s (%d)\n", filename, lfsresult);
}

void cfg_init(void)
{
    cfg_load_with_boot_opt(false);
}

void cfg_set_boot(char *str)
{
    cfg_save_with_boot_opt(str);
}

char *cfg_get_boot(void)
{
    cfg_load_with_boot_opt(true);
    return (char *)mbuf;
}

bool cfg_set_phi2_khz(uint32_t freq_khz)
{
    if (freq_khz > RP6502_MAX_PHI2)
        return false;
    if (freq_khz && freq_khz < RP6502_MIN_PHI2)
        return false;
    uint32_t old_val = cfg_phi2_khz;
    cfg_phi2_khz = cpu_validate_phi2_khz(freq_khz);
    bool ok = true;
    if (old_val != cfg_phi2_khz)
    {
        ok = cpu_set_phi2_khz(cfg_phi2_khz);
        if (ok)
            cfg_save_with_boot_opt(NULL);
    }
    return ok;
}

// Returns actual 6502 frequency adjusted for quantization.
uint32_t cfg_get_phi2_khz(void)
{
    return cpu_validate_phi2_khz(cfg_phi2_khz);
}

// Specify a minimum time for reset low. 0=auto
void cfg_set_reset_ms(uint8_t ms)
{
    if (cfg_reset_ms != ms)
    {
        cfg_reset_ms = ms;
        cfg_save_with_boot_opt(NULL);
    }
}

uint8_t cfg_get_reset_ms(void)
{
    return cfg_reset_ms;
}

void cfg_set_caps(uint8_t mode)
{
    if (mode <= 2 && cfg_caps != mode)
    {
        cfg_caps = mode;
        cfg_save_with_boot_opt(NULL);
    }
}

uint8_t cfg_get_caps(void)
{
    return cfg_caps;
}

bool cfg_set_time_zone(const char *tz)
{
    if (strlen(tz) < sizeof(cfg_time_zone) - 1)
    {
        const char *time_zone = clk_set_time_zone(tz);
        if (strcmp(cfg_time_zone, time_zone))
        {
            strcpy(cfg_time_zone, time_zone);
            cfg_save_with_boot_opt(NULL);
        }
        return true;
    }
    return false;
}

const char *cfg_get_time_zone(void)
{
    return cfg_time_zone;
}

bool cfg_set_codepage(uint32_t cp)
{
    if (cp > UINT16_MAX)
        return false;
    uint32_t old_val = cfg_codepage;
    cfg_codepage = oem_set_codepage(cp);
    if (old_val != cfg_codepage)
        cfg_save_with_boot_opt(NULL);
    return true;
}

uint16_t cfg_get_codepage(void)
{
    return cfg_codepage;
}

bool cfg_set_vga(uint8_t disp)
{
    bool ok = true;
    if (disp <= 2 && cfg_vga_display != disp)
    {
        cfg_vga_display = disp;
        ok = vga_set_vga(cfg_vga_display);
        if (ok)
            cfg_save_with_boot_opt(NULL);
    }
    return ok;
}

uint8_t cfg_get_vga(void)
{
    return cfg_vga_display;
}

#ifdef RP6502_RIA_W

bool cfg_set_rf(uint8_t rf)
{
    bool ok = true;
    if (rf <= 1 && cfg_net_rf != rf)
    {
        cfg_net_rf = rf;
        wfi_disconnect();
        cfg_save_with_boot_opt(NULL);
    }
    return ok;
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
            strcpy(cfg_net_ssid, ssid);
            wfi_disconnect();
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
            wfi_disconnect();
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

#endif /* RP6502_RIA_W */
