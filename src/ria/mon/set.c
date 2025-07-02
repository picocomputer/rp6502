/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "str.h"
#include "sys/cfg.h"
#include "sys/cpu.h"
#include "sys/lfs.h"

static void set_print_phi2(void)
{
    uint32_t phi2_khz = cfg_get_phi2_khz();
    printf("PHI2: %ld kHz", phi2_khz);
    if (phi2_khz < RP6502_MIN_PHI2 || phi2_khz > RP6502_MAX_PHI2)
        printf(" (!!!)");
    printf("\n");
}

static void set_phi2(const char *args, size_t len)
{
    uint32_t val;
    if (len)
    {
        if (!parse_uint32(&args, &len, &val) ||
            !parse_end(args, len))
        {
            printf("?invalid argument\n");
            return;
        }
        if (!cfg_set_phi2_khz(val))
        {
            printf("?invalid speed\n");
            return;
        }
    }
    set_print_phi2();
}

static void set_print_boot(void)
{
    const char *rom = cfg_get_boot();
    if (!rom[0])
        rom = "(none)";
    printf("BOOT: %s\n", rom);
}

static void set_boot(const char *args, size_t len)
{
    if (len)
    {
        char lfs_name[LFS_NAME_MAX + 1];
        if (args[0] == '-' && parse_end(++args, --len))
        {
            cfg_set_boot("");
        }
        else if (parse_rom_name(&args, &len, lfs_name) &&
                 parse_end(args, len))
        {
            struct lfs_info info;
            if (lfs_stat(&lfs_volume, lfs_name, &info) < 0)
            {
                printf("?ROM not installed\n");
                return;
            }
            cfg_set_boot(lfs_name);
        }
        else
        {
            printf("?Invalid ROM name\n");
            return;
        }
    }
    set_print_boot();
}

static void set_print_caps(void)
{
    const char *const caps_labels[] = {"normal", "inverted", "forced"};
    printf("CAPS: %s\n", caps_labels[cfg_get_caps()]);
}

static void set_caps(const char *args, size_t len)
{
    uint32_t val;
    if (len)
    {
        if (parse_uint32(&args, &len, &val) &&
            parse_end(args, len))
        {
            cfg_set_caps(val);
        }
        else
        {
            printf("?invalid argument\n");
            return;
        }
    }
    set_print_caps();
}

static void set_print_code_page()
{
#if (RP6502_CODE_PAGE)
    printf("CP  : %d (dev)\n", RP6502_CODE_PAGE);
#else
    printf("CP  : %d\n", cfg_get_codepage());
#endif
}

static void set_code_page(const char *args, size_t len)
{
    uint32_t val;
    if (len)
    {
        if (!parse_uint32(&args, &len, &val) ||
            !parse_end(args, len) ||
            !cfg_set_codepage(val))
        {
            printf("?invalid argument\n");
            return;
        }
    }
    set_print_code_page();
}

static void set_print_vga(void)
{
    const char *const vga_labels[] = {"640x480", "640x480 and 1280x720", "1280x1024"};
    printf("VGA : %s\n", vga_labels[cfg_get_vga()]);
}

static void set_vga(const char *args, size_t len)
{
    uint32_t val;
    if (len)
    {
        if (parse_uint32(&args, &len, &val) &&
            parse_end(args, len))
        {
            cfg_set_vga(val);
        }
        else
        {
            printf("?invalid argument\n");
            return;
        }
    }
    set_print_vga();
}

#ifdef RP6502_RIA_W

static void set_print_rf(void)
{
    printf("RF  : %s\n", cfg_get_rf() ? "On" : "Off");
}

static void set_rf(const char *args, size_t len)
{
    uint32_t val;
    if (len)
    {
        if (parse_uint32(&args, &len, &val) &&
            parse_end(args, len))
        {
            cfg_set_rf(val);
        }
        else
        {
            printf("?invalid argument\n");
            return;
        }
    }
    set_print_rf();
}

static void set_print_rfcc(void)
{
    const char *cc = cfg_get_rfcc();
    printf("RFCC: %s\n", strlen(cc) ? cc : "Worldwide");
}

static void set_rfcc(const char *args, size_t len)
{
    char rfcc[3];
    if (len)
    {
        if (args[0] == '-' && parse_end(++args, --len))
        {
            cfg_set_rfcc("");
        }
        else if (!parse_string(&args, &len, rfcc, sizeof(rfcc)) ||
                 !parse_end(args, len) ||
                 !cfg_set_rfcc(rfcc))
        {
            printf("?invalid argument\n");
            return;
        }
    }
    set_print_rfcc();
}

static void set_print_ssid(void)
{
    const char *cc = cfg_get_ssid();
    printf("SSID: %s\n", strlen(cc) ? cc : "(none)");
}

static void set_ssid(const char *args, size_t len)
{
    char ssid[33];
    if (len)
    {
        if (args[0] == '-' && parse_end(++args, --len))
        {
            cfg_set_ssid("");
        }
        else if (!parse_string(&args, &len, ssid, sizeof(ssid)) ||
                 !parse_end(args, len) ||
                 !cfg_set_ssid(ssid))
        {
            printf("?invalid argument\n");
            return;
        }
    }
    set_print_ssid();
}

static void set_print_pass(void)
{
    const char *pass = cfg_get_pass();
    printf("PASS: %s\n", strlen(pass) ? "(set)" : "(none)");
}

static void set_pass(const char *args, size_t len)
{
    char pass[65];
    if (len)
    {
        if (args[0] == '-' && parse_end(++args, --len))
        {
            cfg_set_pass("");
        }
        else if (!parse_string(&args, &len, pass, sizeof(pass)) ||
                 !parse_end(args, len) ||
                 !cfg_set_pass(pass))
        {
            printf("?invalid argument\n");
            return;
        }
    }
    set_print_pass();
}

#endif

static void set_print_time_zone(void)
{
    printf("TZ  : %s\n", cfg_get_time_zone());
}

static void set_time_zone(const char *args, size_t len)
{
    char tz[65];
    if (len)
    {
        if (!parse_string(&args, &len, tz, sizeof(tz)) ||
            !parse_end(args, len) ||
            !cfg_set_time_zone(tz))
        {
            printf("?invalid argument\n");
            return;
        }
    }
    set_print_time_zone();
}

typedef void (*set_function)(const char *, size_t);
static struct
{
    size_t attr_len;
    const char *const attr;
    set_function func;
} const SETTERS[] = {
    {4, "caps", set_caps},
    {4, "phi2", set_phi2},
    {4, "boot", set_boot},
    {2, "tz", set_time_zone},
    {2, "cp", set_code_page},
    {3, "vga", set_vga},
#ifdef RP6502_RIA_W
    {2, "rf", set_rf},
    {4, "rfcc", set_rfcc},
    {4, "ssid", set_ssid},
    {4, "pass", set_pass},
#endif
};
static const size_t SETTERS_COUNT = sizeof SETTERS / sizeof *SETTERS;

static void set_print_all(void)
{
    set_print_phi2();
    set_print_caps();
    set_print_boot();
    set_print_time_zone();
    set_print_code_page();
    set_print_vga();
#ifdef RP6502_RIA_W
    set_print_rf();
    set_print_rfcc();
    set_print_ssid();
    set_print_pass();
#endif
}

void set_mon_set(const char *args, size_t len)
{
    if (!len)
        return set_print_all();

    size_t i = 0;
    for (; i < len; i++)
        if (args[i] == ' ')
            break;
    size_t attr_len = i;
    for (; i < len; i++)
        if (args[i] != ' ')
            break;
    size_t args_start = i;
    for (i = 0; i < SETTERS_COUNT; i++)
    {
        if (attr_len == SETTERS[i].attr_len &&
            !strncasecmp(args, SETTERS[i].attr, attr_len))
        {
            SETTERS[i].func(&args[args_start], len - args_start);
            return;
        }
    }
    printf("?Unknown attribute\n");
}
