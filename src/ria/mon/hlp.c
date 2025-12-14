/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "hid/kbd.h"
#include "mon/hlp.h"
#include "mon/mon.h"
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

__in_flash("hlp_commands") static struct
{
    const char *const cmd;
    const char *const text;
} const HLP_COMMANDS[] = {
    {STR_SET, STR_HELP_SET},
    {STR_STATUS, STR_HELP_STATUS},
    {STR_ABOUT, STR_HELP_ABOUT},
    {STR_CREDITS, STR_HELP_ABOUT},
    {STR_SYSTEM, STR_HELP_SYSTEM},
    {STR_0, STR_HELP_SYSTEM},
    {STR_0000, STR_HELP_SYSTEM},
    {STR_LS, STR_HELP_DIR},
    {STR_DIR, STR_HELP_DIR},
    {STR_CD, STR_HELP_DIR},
    {STR_CHDIR, STR_HELP_DIR},
    {STR_MKDIR, STR_HELP_MKDIR},
    {STR_0_COLON, STR_HELP_DIR},
    {STR_1_COLON, STR_HELP_DIR},
    {STR_2_COLON, STR_HELP_DIR},
    {STR_3_COLON, STR_HELP_DIR},
    {STR_4_COLON, STR_HELP_DIR},
    {STR_5_COLON, STR_HELP_DIR},
    {STR_6_COLON, STR_HELP_DIR},
    {STR_7_COLON, STR_HELP_DIR},
    {STR_8_COLON, STR_HELP_DIR},
    {STR_9_COLON, STR_HELP_DIR},
    {STR_LOAD, STR_HELP_LOAD},
    {STR_INFO, STR_HELP_LOAD},
    {STR_INSTALL, STR_HELP_INSTALL},
    {STR_REMOVE, STR_HELP_INSTALL},
    {STR_REBOOT, STR_HELP_REBOOT},
    {STR_RESET, STR_HELP_RESET},
    {STR_UPLOAD, STR_HELP_UPLOAD},
    {STR_UNLINK, STR_HELP_UNLINK},
    {STR_BINARY, STR_HELP_BINARY},
};
static const size_t COMMANDS_COUNT = sizeof HLP_COMMANDS / sizeof *HLP_COMMANDS;

__in_flash("hlp_settings") static struct
{
    const char *const cmd;
    const char *const text;
} const HLP_SETTINGS[] = {
    {STR_PHI2, STR_HELP_SET_PHI2},
    {STR_BOOT, STR_HELP_SET_BOOT},
    {STR_TZ, STR_HELP_SET_TZ},
    {STR_KB, STR_HELP_SET_KB},
    {STR_CP, STR_HELP_SET_CP},
    {STR_VGA, STR_HELP_SET_VGA},
#ifdef RP6502_RIA_W
    {STR_RF, STR_HELP_SET_RF},
    {STR_RFCC, STR_HELP_SET_RFCC},
    {STR_SSID, STR_HELP_SET_SSID},
    {STR_PASS, STR_HELP_SET_PASS},
    {STR_BLE, STR_HELP_SET_BLE},
#endif
};
static const size_t SETTINGS_COUNT = sizeof HLP_SETTINGS / sizeof *HLP_SETTINGS;

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
    puts(STR_HELP_HELP);
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
    if (cmd_len == strlen(STR_SET) && !strncasecmp(args, STR_SET, cmd_len))
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
            return STR_HELP_SET;
        for (size_t i = 0; i < SETTINGS_COUNT; i++)
            if (set_len == strlen(HLP_SETTINGS[i].cmd))
                if (!strncasecmp(args, HLP_SETTINGS[i].cmd, set_len))
                    return HLP_SETTINGS[i].text;
        return NULL;
    }
    // Help for commands and a couple special words.
    for (size_t i = 1; i < COMMANDS_COUNT; i++)
        if (cmd_len == strlen(HLP_COMMANDS[i].cmd))
            if (!strncasecmp(args, HLP_COMMANDS[i].cmd, cmd_len))
                return HLP_COMMANDS[i].text;
    return NULL;
}

static const char *hlp_response_ptr;

static int hlp_response(char *buf, size_t buf_size, int state)
{
    (void)buf;
    (void)buf_size;
    (void)state;
    puts(hlp_response_ptr);
    if (hlp_response_ptr == STR_HELP_ABOUT)
        vip_print();
    if (hlp_response_ptr == STR_HELP_SET_KB)
        kbd_print_layouts();
    return -1;
}

void hlp_mon_help(const char *args, size_t len)
{
    if (!len)
        return hlp_help(args, len);
    while (len && args[len - 1] == ' ')
        len--;
    hlp_response_ptr = help_text_lookup(args, len);
    if (hlp_response_ptr)
        mon_set_response_fn(hlp_response);
    else
        rom_mon_help(args, len);
}

bool hlp_topic_exists(const char *buf, size_t buflen)
{
    return !!help_text_lookup(buf, buflen);
}
