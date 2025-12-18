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

// Optional string can replace boot string
static void cfg_save_with_boot_opt(const char *opt_str)
{
    lfs_file_t lfs_file;
    LFS_FILE_CONFIG(lfs_file_config);
    int lfsresult = lfs_file_opencfg(&lfs_volume, &lfs_file, STR_CFG_FILENAME,
                                     LFS_O_RDWR | LFS_O_CREAT,
                                     &lfs_file_config);
    mon_add_response_lfs(lfsresult);
    if (lfsresult < 0)
        return;
    if (!opt_str)
    {
        opt_str = (char *)mbuf;
        // Fetch the boot string, ignore the rest
        while (lfs_gets((char *)mbuf, MBUF_SIZE, &lfs_volume, &lfs_file))
            if (mbuf[0] != '+')
                break;
        lfsresult = lfs_file_rewind(&lfs_volume, &lfs_file);
        mon_add_response_lfs(lfsresult);
    }
    if (lfsresult >= 0)
        lfsresult = lfs_file_truncate(&lfs_volume, &lfs_file, 0);
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
                               vga_get_display_type(),
#ifdef RP6502_RIA_W
                               cyw_get_rf_enable(),
                               cyw_get_rf_country_code(),
                               wfi_get_ssid(),
                               wfi_get_pass(),
                               ble_get_enabled(),
#endif /* RP6502_RIA_W */
                               opt_str);
    }
    mon_add_response_lfs(lfsresult);
    int lfscloseresult = lfs_file_close(&lfs_volume, &lfs_file);
    mon_add_response_lfs(lfscloseresult);
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
            mon_add_response_lfs(lfsresult);
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
            vga_load_display_type(str, len);
            break;
#ifdef RP6502_RIA_W
        case 'E':
            cyw_load_rf_enable(str, len);
            break;
        case 'F':
            cyw_load_rf_country_code(str, len);
            break;
        case 'W':
            wfi_load_ssid(str, len);
            break;
        case 'K':
            wfi_load_pass(str, len);
            break;
        case 'B':
            ble_load_enabled(str, len);
            break;
#endif /* RP6502_RIA_W */
        default:
            break;
        }
    }
    lfsresult = lfs_file_close(&lfs_volume, &lfs_file);
    mon_add_response_lfs(lfsresult);
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
