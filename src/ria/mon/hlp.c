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

static const char *hlp_response_ptr;
static int (*hlp_response_next_fn)(char *, size_t, int);

__in_flash("hlp_commands") static struct
{
    const char *const cmd;
    const char *const text;
    int (*extra_fn)(char *, size_t, int);
} const HLP_COMMANDS[] = {
    {STR_SET, STR_HELP_SET, NULL},
    {STR_STATUS, STR_HELP_STATUS, NULL},
    {STR_ABOUT, STR_HELP_ABOUT, vip_response},
    {STR_CREDITS, STR_HELP_ABOUT, vip_response},
    {STR_SYSTEM, STR_HELP_SYSTEM, NULL},
    {STR_0, STR_HELP_SYSTEM, NULL},
    {STR_0000, STR_HELP_SYSTEM, NULL},
    {STR_LS, STR_HELP_DIR, NULL},
    {STR_DIR, STR_HELP_DIR, NULL},
    {STR_CD, STR_HELP_DIR, NULL},
    {STR_CHDIR, STR_HELP_DIR, NULL},
    {STR_MKDIR, STR_HELP_MKDIR, NULL},
    {STR_0_COLON, STR_HELP_DIR, NULL},
    {STR_1_COLON, STR_HELP_DIR, NULL},
    {STR_2_COLON, STR_HELP_DIR, NULL},
    {STR_3_COLON, STR_HELP_DIR, NULL},
    {STR_4_COLON, STR_HELP_DIR, NULL},
    {STR_5_COLON, STR_HELP_DIR, NULL},
    {STR_6_COLON, STR_HELP_DIR, NULL},
    {STR_7_COLON, STR_HELP_DIR, NULL},
    {STR_8_COLON, STR_HELP_DIR, NULL},
    {STR_9_COLON, STR_HELP_DIR, NULL},
    {STR_LOAD, STR_HELP_LOAD, NULL},
    {STR_INFO, STR_HELP_LOAD, NULL},
    {STR_INSTALL, STR_HELP_INSTALL, NULL},
    {STR_REMOVE, STR_HELP_INSTALL, NULL},
    {STR_REBOOT, STR_HELP_REBOOT, NULL},
    {STR_RESET, STR_HELP_RESET, NULL},
    {STR_UPLOAD, STR_HELP_UPLOAD, NULL},
    {STR_UNLINK, STR_HELP_UNLINK, NULL},
    {STR_BINARY, STR_HELP_BINARY, NULL},
};
static const size_t COMMANDS_COUNT = sizeof HLP_COMMANDS / sizeof *HLP_COMMANDS;

__in_flash("hlp_settings") static struct
{
    const char *const cmd;
    const char *const text;
    int (*extra_fn)(char *, size_t, int);
} const HLP_SETTINGS[] = {
    {STR_PHI2, STR_HELP_SET_PHI2, NULL},
    {STR_BOOT, STR_HELP_SET_BOOT, NULL},
    {STR_TZ, STR_HELP_SET_TZ, NULL},
    {STR_KB, STR_HELP_SET_KB, kbd_layouts_response},
    {STR_CP, STR_HELP_SET_CP, NULL},
    {STR_VGA, STR_HELP_SET_VGA, NULL},
#ifdef RP6502_RIA_W
    {STR_RF, STR_HELP_SET_RF, NULL},
    {STR_RFCC, STR_HELP_SET_RFCC, NULL},
    {STR_SSID, STR_HELP_SET_SSID, NULL},
    {STR_PASS, STR_HELP_SET_PASS, NULL},
    {STR_BLE, STR_HELP_SET_BLE, NULL},
#endif
};
static const size_t SETTINGS_COUNT = sizeof HLP_SETTINGS / sizeof *HLP_SETTINGS;

static void help_response_lookup(const char *args, size_t len)
{
    hlp_response_ptr = NULL;
    hlp_response_next_fn = NULL;
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
        {
            hlp_response_ptr = STR_HELP_SET;
            return;
        }
        for (size_t i = 0; i < SETTINGS_COUNT; i++)
            if (set_len == strlen(HLP_SETTINGS[i].cmd))
                if (!strncasecmp(args, HLP_SETTINGS[i].cmd, set_len))
                {
                    hlp_response_ptr = HLP_SETTINGS[i].text;
                    hlp_response_next_fn = HLP_SETTINGS[i].extra_fn;
                    return;
                }
        return;
    }
    // Help for commands and a couple special words.
    for (size_t i = 1; i < COMMANDS_COUNT; i++)
        if (cmd_len == strlen(HLP_COMMANDS[i].cmd))
            if (!strncasecmp(args, HLP_COMMANDS[i].cmd, cmd_len))
            {
                hlp_response_ptr = HLP_COMMANDS[i].text;
                hlp_response_next_fn = HLP_COMMANDS[i].extra_fn;
                return;
            }
    return;
}

static int hlp_response(char *buf, size_t buf_size, int state)
{
    (void)state;
    size_t i;
    for (i = 0; i < buf_size - 1; i++)
    {
        char c = hlp_response_ptr[i];
        buf[i] = c;
        if (!c)
        {
            if (hlp_response_next_fn)
            {
                mon_set_response_fn(hlp_response_next_fn);
                return 0;
            }
            return -1;
        }
        if (c == '\n')
        {
            i++;
            break;
        }
    }
    buf[i] = 0;
    hlp_response_ptr += i;
    return 0;
}

void hlp_mon_help(const char *args, size_t len)
{
    if (!len)
    {
        hlp_response_ptr = STR_HELP_HELP;
        hlp_response_next_fn = rom_installed_response;
        mon_set_response_fn(hlp_response);
    }
    else
    {
        while (len && args[len - 1] == ' ')
            len--;
        help_response_lookup(args, len);
    }
    if (hlp_response_ptr)
        mon_set_response_fn(hlp_response);
    else
        rom_mon_help(args, len);
}

bool hlp_topic_exists(const char *buf, size_t buflen)
{
    help_response_lookup(buf, buflen);
    return hlp_response_ptr != NULL;
}
