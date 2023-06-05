/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "str.h"
#include "sys/cfg.h"
#include "sys/cpu.h"
#include "sys/lfs.h"
#include "usb/usb.h"
#include "hardware/clocks.h"

static void status_phi2()
{
    uint32_t phi2_khz = cfg_get_phi2_khz();
    printf("PHI2: %ld kHz", phi2_khz);
    if (phi2_khz < 50)
        printf(" (!!!)");
    printf("\n");
}

static void set_phi2(const char *args, size_t len)
{
    uint32_t val;
    if (len)
    {
        if (!parse_uint32(&args, &len, &val) ||
            !parse_end(args, len) ||
            !cfg_set_phi2_khz(val))
        {
            printf("?invalid argument\n");
            return;
        }
    }
    status_phi2();
}

static void status_resb()
{
    uint8_t reset_ms = cfg_get_reset_ms();
    float reset_us = cpu_get_reset_us();
    if (!reset_ms)
        printf("RESB: %.3f ms (auto)\n", reset_us / 1000.f);
    else if (reset_ms * 1000 == reset_us)
        printf("RESB: %d ms\n", reset_ms);
    else
        printf("RESB: %.0f ms (%d ms requested)\n", reset_us / 1000.f, reset_ms);
}

static void set_resb(const char *args, size_t len)
{
    uint32_t val;
    if (len)
    {
        if (parse_uint32(&args, &len, &val) &&
            parse_end(args, len))
        {
            if (val > 255)
            {
                printf("?invalid duration\n");
                return;
            }
            cfg_set_reset_ms(val);
        }
        else
        {
            printf("?invalid argument\n");
            return;
        }
    }
    status_resb();
}

static void status_boot()
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
    status_boot();
}

static void status_caps()
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
    status_caps();
}

static void status_code_page()
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
    status_code_page();
}

static void status_vga()
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
    status_vga();
}

void set_status(const char *args, size_t len)
{
    (void)(args);
    (void)(len);

    status_phi2();
    status_resb();
    status_caps();
    status_boot();
    status_code_page();
    status_vga();
    printf("RIA : %.1f MHz\n", clock_get_hz(clk_sys) / 1000 / 1000.f);
    usb_print_status();
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
    {4, "resb", set_resb},
    {4, "boot", set_boot},
    {2, "cp", set_code_page},
    {3, "vga", set_vga},
};
static const size_t SETTERS_COUNT = sizeof SETTERS / sizeof *SETTERS;

void set_attr(const char *args, size_t len)
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
    for (i = 0; i < SETTERS_COUNT; i++)
    {
        if (attr_len == SETTERS[i].attr_len &&
            !strnicmp(args, SETTERS[i].attr, attr_len))
        {
            SETTERS[i].func(&args[args_start], len - args_start);
            return;
        }
    }
    printf("?Unknown attribute\n");
}
