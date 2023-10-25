/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "str.h"
#include "api/oem.h"
#include "sys/cfg.h"
#include "sys/cpu.h"
#include "sys/lfs.h"
#include "sys/mem.h"
#include "sys/vga.h"

// Configuration is a plain ASCII file on the LFS. e.g.
// +V1         | Version - Must be first
// +P8000      | PHI2
// +C0         | Caps
// +R0         | RESB
// +S437       | Code Page
// +D0         | VGA display type
// BASIC       | Boot ROM - Must be last

#define CFG_VERSION 1
static const char filename[] = "CONFIG.SYS";

static uint32_t cfg_phi2_khz;
static uint8_t cfg_reset_ms;
static uint8_t cfg_caps;
static uint32_t cfg_codepage;
static uint8_t cfg_vga_display;

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
                               "+S%d\n"
                               "+D%d\n"
                               "%s",
                               CFG_VERSION,
                               cfg_phi2_khz,
                               cfg_reset_ms,
                               cfg_caps,
                               cfg_codepage,
                               cfg_vga_display,
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
        size_t len = strlen((char *)mbuf);
        while (len && mbuf[len - 1] == '\n')
            len--;
        mbuf[len] = 0;
        if (len < 3 || mbuf[0] != '+')
            break;
        const char *str = (char *)mbuf + 2;
        len -= 2;
        uint32_t val;
        if (!boot_only && parse_uint32(&str, &len, &val))
            switch (mbuf[1])
            {
            case 'P':
                cfg_phi2_khz = val;
                break;
            case 'R':
                cfg_reset_ms = val;
                break;
            case 'C':
                cfg_caps = val;
                break;
            case 'S':
                cfg_codepage = val;
                break;
            case 'D':
                cfg_vga_display = val;
                break;
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
