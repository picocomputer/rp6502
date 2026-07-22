/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ria/api/tim.h"
#include "ria/hid/kbd.h"
#include "ria/mon/hlp.h"
#include "ria/mon/mon.h"
#include "ria/mon/rom.h"
#include "ria/net/cyw.h"
#include "ria/net/wfi.h"
#include "ria/str/str.h"
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
    {STR_TZ, STR_HELP_SET_TZ, tim_tzdata_response},
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

static const char *hlp_find(const hlp_entry_t *tbl, size_t n,
                            const char *key, mon_response_fn *fn)
{
    for (size_t i = 0; i < n; i++)
        if (!strcasecmp(key, tbl[i].cmd))
        {
            if (fn)
                *fn = tbl[i].extra_fn;
            return S(tbl[i].prose);
        }
    return NULL;
}

const char *hlp_lookup(const char *word, const char *sub, mon_response_fn *fn)
{
    if (fn)
        *fn = NULL;
    if (!word)
        return NULL;
    // SET and DISK are the only commands with a second level of help.
    if (sub)
    {
        if (!strcasecmp(word, STR_SET))
            return hlp_find(HLP_SETTINGS, HLP_SETTINGS_COUNT, sub, fn);
        if (!strcasecmp(word, STR_DISK))
            return hlp_find(HLP_DISK, HLP_DISK_COUNT, sub, fn);
        return NULL;
    }
    // ABOUT and CREDITS share the non-localized credits help.
    if (!strcasecmp(word, STR_ABOUT) || !strcasecmp(word, STR_CREDITS))
        return STR_HELP_ABOUT;
    return hlp_find(HLP_COMMANDS, HLP_COMMANDS_COUNT, word, fn);
}

// Split a help query into its command word and optional SET/DISK sub-key. word
// is copied out because str_parse_string reuses one shared buffer, so parsing
// the sub-key would otherwise clobber word.
static void hlp_split(const char *args, char *word, size_t word_size, const char **sub)
{
    const char *tok = str_parse_string(&args);
    strncpy(word, tok ? tok : "", word_size - 1);
    word[word_size - 1] = 0;
    *sub = NULL;
    if (!strcasecmp(word, STR_SET) || !strcasecmp(word, STR_DISK))
        *sub = str_parse_string(&args);
}

void hlp_mon_help(const char *args)
{
    if (!*args)
    {
        mon_add_response_utf8(S(STR_HELP_HELP));
        mon_add_response_fn(rom_installed_response);
        return;
    }
    char word[16];
    const char *sub;
    hlp_split(args, word, sizeof word, &sub);
    mon_response_fn fn;
    const char *prose = hlp_lookup(word, sub, &fn);
    if (!prose)
    {
        rom_mon_help(args);
        return;
    }
    mon_add_response_utf8(prose);
#ifdef RP6502_RIA_W
    // Radio builds continue the settings summary and the credits with a _W block.
    if (!sub && !strcasecmp(word, STR_SET))
        mon_add_response_utf8(S(STR_HELP_SET_W));
    else if (!strcasecmp(word, STR_ABOUT) || !strcasecmp(word, STR_CREDITS))
        mon_add_response_utf8(STR_HELP_ABOUT_W);
#endif
    if (fn != NULL)
        mon_add_response_fn(fn);
}

bool hlp_topic_exists(const char *buf)
{
    char word[16];
    const char *sub;
    hlp_split(buf, word, sizeof word, &sub);
    return hlp_lookup(word, sub, NULL) != NULL;
}
