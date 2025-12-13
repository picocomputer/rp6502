/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "hid/kbd.h"
#include "mon/hlp.h"
#include "mon/rom.h"
#include "mon/vip.h"
#include "str/str.h"
#include "sys/cpu.h"
#include "sys/lfs.h"
#include <ctype.h>

#if defined(DEBUG_RIA_MON) || defined(DEBUG_RIA_MON_HLP)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define FREQS STR_STRINGIFY(CPU_PHI2_MIN_KHZ) "-" STR_STRINGIFY(CPU_PHI2_MAX_KHZ)

static const char __in_flash("helptext") hlp_text_set_phi2[] =
    "PHI2 is the 6502 clock speed in kHz. The valid range is " FREQS " but not all\n"
    "frequencies are available. In that case, the next highest frequency will\n"
    "be automatically calculated and selected. Setting is saved on the RIA flash.";

static const char __in_flash("helptext") hlp_text_set_boot[] =
    "BOOT selects an installed ROM to be automatically loaded and started when the\n"
    "system is powered up or rebooted. For example, you might want the system to\n"
    "immediately boot into BASIC or an operating system CLI. Using \"-\" for the\n"
    "argument will have the system boot into the monitor you are using now.\n"
    "Setting is saved on the RIA flash.";

static const char __in_flash("helptext") hlp_text_set_tz[] =
    "TZ sets the time zone using the same format as POSIX. The default is \"UTC0\".\n"
    "Some examples are \"PST8PDT,M3.2.0/2,M11.1.0/2\" for USA Pacific time and \n"
    "\"CET-1CEST,M3.5.0/2,M10.5.0/3\" for Central European time.\n"
    "The easiest way to get this is to ask an AI \"posix tz for {your location}\".";

static const char __in_flash("helptext") hlp_text_set_kb[] =
    "SET KB selects a keyboard layout. e.g. SET KB US";

static const char __in_flash("helptext") hlp_text_set_cp[] =
    "SET CP selects a code page for system text. The following is supported:\n"
    "437, 720, 737, 771, 775, 850, 852, 855, 857, 860, 861, 862, 863, 864, 865,\n"
    "866, 869, 932, 936, 949, 950.  Code pages 720, 932, 936, 949, 950 do not have\n"
    "VGA fonts."
#if RP6502_CODE_PAGE
    "\nThis is a development build. Only " STR_STRINGIFY(RP6502_CODE_PAGE) " is available.";
#else
    "";
#endif

static const char __in_flash("helptext") hlp_text_set_vga[] =
    "SET VGA selects the display type for VGA output. All canvas resolutions are\n"
    "supported by all display types. Display type is used to maintain square\n"
    "pixels and minimize letterboxing. Note that 1280x1024 is 5:4 so 4:3 graphics\n"
    "will be letterboxed slightly but you'll get 2 extra rows on the terminal.\n"
    "  0 - 640x480, for 4:3 displays\n"
    "  1 - 640x480 and 1280x720, for 16:9 displays\n"
    "  2 - 1280x1024, for 5:4 SXGA displays";

#ifdef RP6502_RIA_W

static const char __in_flash("helptext") hlp_text_set_rf[] =
    "SET RF (0|1) turns all radios off or on.";

static const char __in_flash("helptext") hlp_text_set_rfcc[] =
    "Set this so the CYW43 can use the best radio frequencies for your country.\n"
    "Using \"-\" will clear the country code and default to a worldwide setting.\n"
    "Valid country codes: AU, AT, BE, BR, CA, CL, CN, CO, CZ, DK, EE, FI, FR, DE,\n"
    "GR, HK, HU, IS, IN, IL, IT, JP, KE, LV, LI, LT, LU, MY, MT, MX, NL, NZ, NG,\n"
    "NO, PE, PH, PL, PT, SG, SK, SI, ZA, KR, ES, SE, CH, TW, TH, TR, GB, US.";

static const char __in_flash("helptext") hlp_text_set_ssid[] =
    "This is the Service Set Identifier for your WiFi network. Setting \"-\" will\n"
    "disable WiFi.";

static const char __in_flash("helptext") hlp_text_set_pass[] =
    "This is the password for your WiFi network. Use \"-\" to clear password.";

static const char __in_flash("helptext") hlp_text_set_ble[] =
    "Setting 0 disables Bluetooth LE. Setting 1 enables. Setting 2 enters pairing\n"
    "mode which will remain active until successful.";

#endif

static struct
{
    size_t cmd_len;
    const char *const cmd;
    const char *const text;
} const COMMANDS[] = {
    {3, "set", hlp_text_set}, // must be first
    {6, "status", hlp_text_status},
    {5, "about", hlp_text_about},
    {7, "credits", hlp_text_about},
    {6, "system", hlp_text_system},
    {1, "0", hlp_text_system},
    {4, "0000", hlp_text_system},
    {2, "ls", hlp_text_dir},
    {3, "dir", hlp_text_dir},
    {2, "cd", hlp_text_dir},
    {5, "chdir", hlp_text_dir},
    {5, "mkdir", hlp_text_mkdir},
    {2, "0:", hlp_text_dir},
    {2, "1:", hlp_text_dir},
    {2, "2:", hlp_text_dir},
    {2, "3:", hlp_text_dir},
    {2, "4:", hlp_text_dir},
    {2, "5:", hlp_text_dir},
    {2, "6:", hlp_text_dir},
    {2, "7:", hlp_text_dir},
    {2, "8:", hlp_text_dir},
    {2, "9:", hlp_text_dir},
    {4, "load", hlp_text_load},
    {4, "info", hlp_text_load},
    {7, "install", hlp_text_install},
    {6, "remove", hlp_text_install},
    {6, "reboot", hlp_text_reboot},
    {5, "reset", hlp_text_reset},
    {6, "upload", hlp_text_upload},
    {6, "unlink", hlp_text_unlink},
    {6, "binary", hlp_text_binary},
};
static const size_t COMMANDS_COUNT = sizeof COMMANDS / sizeof *COMMANDS;

static struct
{
    size_t set_len;
    const char *const cmd;
    const char *const text;
} const SETTINGS[] = {
    {4, "phi2", hlp_text_set_phi2},
    {4, "boot", hlp_text_set_boot},
    {2, "tz", hlp_text_set_tz},
    {2, "kb", hlp_text_set_kb},
    {2, "cp", hlp_text_set_cp},
    {3, "vga", hlp_text_set_vga},
#ifdef RP6502_RIA_W
    {2, "rf", hlp_text_set_rf},
    {4, "rfcc", hlp_text_set_rfcc},
    {4, "ssid", hlp_text_set_ssid},
    {4, "pass", hlp_text_set_pass},
    {3, "ble", hlp_text_set_ble},
#endif
};
static const size_t SETTINGS_COUNT = sizeof SETTINGS / sizeof *SETTINGS;

// Use width=0 to supress printing. Returns count.
// Anything with only uppercase letters is counted.
static uint32_t hlp_roms_list(uint32_t width)
{
    uint32_t count = 0;
    uint32_t col = 0;
    lfs_dir_t lfs_dir;
    struct lfs_info lfs_info;
    int result = lfs_dir_open(&lfs_volume, &lfs_dir, "/");
    if (result < 0)
    {
        printf("?Unable to open ROMs directory (%d)\n", result);
        return 0;
    }
    while (true)
    {
        result = lfs_dir_read(&lfs_volume, &lfs_dir, &lfs_info);
        if (!result)
            break;
        if (result < 0)
        {
            printf("?Error reading ROMs directory (%d)\n", result);
            count = 0;
            break;
        }
        bool is_ok = true;
        size_t len = strlen(lfs_info.name);
        for (size_t i = 0; i < len; i++)
        {
            char ch = lfs_info.name[i];
            if (!(i && isdigit(ch)) && !isupper(ch))
                is_ok = false;
        }
        if (is_ok && width)
        {
            if (count)
            {
                putchar(',');
                col += 1;
            }
            if (col + len > width - 2)
            {
                printf("\n%s", lfs_info.name);
                col = len;
            }
            else
            {
                if (col)
                {
                    putchar(' ');
                    col += 1;
                }
                printf("%s", lfs_info.name);
                col += len;
            }
        }
        if (is_ok)
            count++;
    }
    if (width)
    {
        if (count)
        {
            putchar('.');
            col++;
        }
        putchar('\n');
    }
    result = lfs_dir_close(&lfs_volume, &lfs_dir);
    if (result < 0)
    {
        printf("?Error closing ROMs directory (%d)\n", result);
        count = 0;
    }
    return count;
}

static void hlp_help(const char *args, size_t len)
{
    (void)(args);
    (void)(len);
    puts(STR_HLP_HELP);
    uint32_t rom_count = hlp_roms_list(0);
    if (rom_count)
    {
        printf("%ld installed ROM%s:\n", rom_count, rom_count == 1 ? "" : "s");
        hlp_roms_list(79);
    }
    else
        printf("No installed ROMs.\n");
}

// Returns NULL if not found.
static const char *help_text_lookup(const char *args, size_t len)
{
    size_t cmd_len;
    for (cmd_len = 0; cmd_len < len; cmd_len++)
        if (args[cmd_len] == ' ')
            break;
    // SET command has another level of help
    if (cmd_len == COMMANDS[0].cmd_len && !strncasecmp(args, COMMANDS[0].cmd, cmd_len))
    {
        args += cmd_len;
        len -= cmd_len;
        while (len && args[0] == ' ')
            args++, len--;
        size_t set_len;
        for (set_len = 0; set_len < len; set_len++)
            if (args[set_len] == ' ')
                break;
        if (!set_len)
            return COMMANDS[0].text;
        for (size_t i = 0; i < SETTINGS_COUNT; i++)
            if (set_len == SETTINGS[i].set_len)
                if (!strncasecmp(args, SETTINGS[i].cmd, set_len))
                    return SETTINGS[i].text;
        return NULL;
    }
    // Help for commands and a couple special words.
    for (size_t i = 1; i < COMMANDS_COUNT; i++)
        if (cmd_len == COMMANDS[i].cmd_len)
            if (!strncasecmp(args, COMMANDS[i].cmd, cmd_len))
                return COMMANDS[i].text;
    return NULL;
}

void hlp_mon_help(const char *args, size_t len)
{
    if (!len)
        return hlp_help(args, len);
    while (len && args[len - 1] == ' ')
        len--;
    const char *text = help_text_lookup(args, len);
    if (text)
    {
        puts(text);
        if (text == hlp_text_about)
            vip_print();
        if (text == hlp_text_set_kb)
            kbd_print_layouts();
    }
    else
    {
        if (!rom_help(args, len))
            puts("?No help found.");
    }
}

bool hlp_topic_exists(const char *buf, size_t buflen)
{
    return !!help_text_lookup(buf, buflen);
}
