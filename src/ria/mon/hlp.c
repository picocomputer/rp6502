/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "api/clk.h"
#include "hid/kbd.h"
#include "mon/hlp.h"
#include "mon/mon.h"
#include "mon/rom.h"
#include "net/cyw.h"
#include "net/wfi.h"
#include "str/str.h"
#include <pico.h>
#include <string.h>

#if defined(DEBUG_RIA_MON) || defined(DEBUG_RIA_MON_HLP)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

typedef struct
{
    const char *const cmd;
    int prose; // localized string id for S()
    mon_response_fn extra_fn;
} hlp_entry_t;

__in_flash("hlp_commands") static const hlp_entry_t HLP_COMMANDS[] = {
    {STR_SET, STR_HELP_SET, NULL},
    {STR_STATUS, STR_HELP_STATUS, NULL},
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
    {STR_FLASH, STR_HELP_FLASH, NULL},
    {STR_UPLOAD, STR_HELP_UPLOAD, NULL},
    {STR_UNLINK, STR_HELP_UNLINK, NULL},
    {STR_COPY, STR_HELP_COPY, NULL},
    {STR_MOVE, STR_HELP_MOVE, NULL},
    {STR_BINARY, STR_HELP_BINARY, NULL},
    {STR_DISK, STR_HELP_DISK, NULL},
};
static const size_t HLP_COMMANDS_COUNT = sizeof HLP_COMMANDS / sizeof *HLP_COMMANDS;

__in_flash("hlp_settings") static const hlp_entry_t HLP_SETTINGS[] = {
    {STR_PHI2, STR_HELP_SET_PHI2, NULL},
    {STR_BOOT, STR_HELP_SET_BOOT, NULL},
    {STR_TZ, STR_HELP_SET_TZ, clk_tzdata_response},
    {STR_LOC, STR_HELP_SET_LOC, str_locales_response},
    {STR_KB, STR_HELP_SET_KB, kbd_layouts_response},
    {STR_CP, STR_HELP_SET_CP, NULL},
    {STR_VGA, STR_HELP_SET_VGA, NULL},
    {STR_NFC, STR_HELP_SET_NFC, NULL},
#ifdef RP6502_RIA_W
    {STR_RF, STR_HELP_SET_RF, NULL},
    {STR_RFCC, STR_HELP_SET_RFCC, cyw_country_code_response},
    {STR_SSID, STR_HELP_SET_SSID, wfi_scan_response},
    {STR_PASS, STR_HELP_SET_PASS, NULL},
    {STR_BLE, STR_HELP_SET_BLE, NULL},
    {STR_PORT, STR_HELP_SET_PORT, NULL},
    {STR_KEY, STR_HELP_SET_KEY, NULL},
#endif
};
static const size_t HLP_SETTINGS_COUNT = sizeof HLP_SETTINGS / sizeof *HLP_SETTINGS;

__in_flash("hlp_disk") static const hlp_entry_t HLP_DISK[] = {
    {STR_INFO, STR_HELP_DISK_INFO, NULL},
#if RP6502_EXFAT
    {STR_FORMAT, STR_HELP_DISK_FORMAT, NULL},
#else
    {STR_FORMAT, STR_HELP_DISK_FORMAT_BASIC, NULL},
#endif
    {STR_ERASE, STR_HELP_DISK_ERASE, NULL},
    {STR_VERIFY, STR_HELP_DISK_VERIFY, NULL},
    {STR_LABEL, STR_HELP_DISK_LABEL, NULL},
};
static const size_t HLP_DISK_COUNT = sizeof HLP_DISK / sizeof *HLP_DISK;

static bool hlp_find(const hlp_entry_t *tbl, size_t n,
                     const char *key, hlp_topic_t *out)
{
    for (size_t i = 0; i < n; i++)
        if (!strcasecmp(key, tbl[i].cmd))
        {
            out->prose = S(tbl[i].prose);
            out->fn = tbl[i].extra_fn;
            return true;
        }
    return false;
}

bool hlp_lookup(const char *word, const char *sub, hlp_topic_t *out)
{
    out->prose = NULL;
    out->append = NULL;
    out->fn = NULL;
    if (!word)
        return false;
    // SET and DISK are the only commands with a second level of help.
    if (sub)
    {
        if (!strcasecmp(word, STR_SET))
            return hlp_find(HLP_SETTINGS, HLP_SETTINGS_COUNT, sub, out);
        if (!strcasecmp(word, STR_DISK))
            return hlp_find(HLP_DISK, HLP_DISK_COUNT, sub, out);
        return false;
    }
    // ABOUT and CREDITS share the non-localized credits help.
    if (!strcasecmp(word, STR_ABOUT) || !strcasecmp(word, STR_CREDITS))
    {
        out->prose = STR_HELP_ABOUT;
#ifdef RP6502_RIA_W
        out->append = STR_HELP_ABOUT_W;
#endif
        return true;
    }
    if (hlp_find(HLP_COMMANDS, HLP_COMMANDS_COUNT, word, out))
    {
#ifdef RP6502_RIA_W
        if (!strcasecmp(word, STR_SET))
            out->append = S(STR_HELP_SET_W);
#endif
        return true;
    }
    return false;
}

static bool hlp_lookup_args(const char *args, hlp_topic_t *out)
{
    const char *word = str_parse_string(&args);
    const char *sub = NULL;
    if (word && (!strcasecmp(word, STR_SET) || !strcasecmp(word, STR_DISK)))
        sub = str_parse_string(&args);
    return hlp_lookup(word, sub, out);
}

void hlp_mon_help(const char *args)
{
    if (!*args)
    {
        mon_add_response_utf8(S(STR_HELP_HELP));
        mon_add_response_fn(rom_installed_response);
        return;
    }
    hlp_topic_t t;
    if (!hlp_lookup_args(args, &t))
    {
        rom_mon_help(args);
        return;
    }
    mon_add_response_utf8(t.prose);
    if (t.append != NULL)
        mon_add_response_utf8(t.append);
    if (t.fn != NULL)
        mon_add_response_fn(t.fn);
}

bool hlp_topic_exists(const char *buf)
{
    hlp_topic_t t;
    return hlp_lookup_args(buf, &t);
}
