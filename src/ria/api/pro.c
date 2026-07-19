/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ria/api/api.h"
#include "ria/api/arg.h"
#include "ria/api/pro.h"
#include "ria/aud/bel.h"
#include "ria/main.h"
#include "ria/mon/mon.h"
#include "ria/mon/rom.h"
#include "ria/str/rln.h"
#include "ria/str/str.h"
#include "ria/usb/nfc.h"
#include <fatfs/ff.h>
#include <stdio.h>

#if defined(DEBUG_RIA_API) || defined(DEBUG_RIA_API_PRO)
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// Records argv[0] of the currently running process.
static char pro_running_path[256];

// Records the launcher that will re-run when program ends.
static char pro_launcher_path[256];

static int16_t pro_exit_code;

void pro_run(void)
{
    const char *argv0 = arg_index(0);
    if (argv0)
    {
        strncpy(pro_running_path, argv0, sizeof(pro_running_path) - 1);
        pro_running_path[sizeof(pro_running_path) - 1] = '\0';
    }
    else
        pro_running_path[0] = '\0';
}

void pro_stop(void)
{
    pro_exit_code = API_AX;
    if (rom_active())
    {
        // A new ROM load is already in flight (pro_api_exec or pro_nfc);
        // skip the launcher re-exec so we don't clobber it.
        pro_running_path[0] = '\0';
        return;
    }
    bool relaunch = !pro_is_launcher() && pro_launcher_path[0] != '\0';
    pro_running_path[0] = '\0';
    if (!relaunch)
        pro_launcher_path[0] = '\0';
    else
    {
        arg_clear();
        arg_append(pro_launcher_path);
        rom_exec();
    }
}

void pro_cancel_launcher(void)
{
    pro_launcher_path[0] = '\0';
}

bool pro_api_argv(void)
{
    return api_return_ax(arg_push_xstack());
}

bool pro_api_exec(void)
{
    if (!arg_pull_xstack())
        return api_return_errno(API_EINVAL);
    // Committed to the exec; rom.c surfaces any load errors on the console.
    main_stop();
    rom_exec();
    return api_return_ax(0);
}

bool pro_has_launcher(void)
{
    return pro_launcher_path[0] != '\0';
}

void pro_set_launcher(bool is_launcher)
{
    if (is_launcher)
    {
        strncpy(pro_launcher_path, pro_running_path, sizeof(pro_launcher_path) - 1);
        pro_launcher_path[sizeof(pro_launcher_path) - 1] = '\0';
    }
    else
        pro_launcher_path[0] = '\0';
}

bool pro_is_launcher(void)
{
    return pro_launcher_path[0] != '\0' &&
           strcmp(pro_running_path, pro_launcher_path) == 0;
}

int16_t pro_get_exit_code(void)
{
    return pro_exit_code;
}

void pro_nfc(const uint8_t *tag_data, size_t len)
{
    char path[256];
    DBG("pro_nfc(%zu bytes)\n", len);

    if (!nfc_parse_text(tag_data, len, path, sizeof(path)))
        goto fail;
    DBG("pro_nfc text %s\n", path);

    // Parse the first arg for the ROM path. NFC tags address filesystem
    // ROMs only; ':installed' names are rejected.
    const char *args = path;
    const char *first_arg = str_parse_string(&args);
    if (!first_arg || *first_arg == ':')
        goto fail;

    bool has_drive = (strchr(first_arg, ':') != NULL);
    if (has_drive)
    {
        // str_parse_string and str_abs_path share one static buffer, so
        // copy first_arg out of it before calling str_abs_path.
        strncpy(path, first_arg, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
        const char *abs = str_abs_path(path);
        if (!abs)
            goto fail;
        strncpy(path, abs, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
        if (f_stat(path, NULL) != FR_OK)
            goto fail;
    }
    else
    {
        // Build canonical "MSC0:/path"
        const char *p = (*first_arg == '/') ? first_arg + 1 : first_arg;
        snprintf(path, sizeof(path), "MSC0:/%s", p);
        const char *abs = str_abs_path(path);
        if (!abs)
            goto fail;
        strncpy(path, abs, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
        bool found = false;
        for (int drive = 0; drive <= 9; drive++)
        {
            path[3] = (char)('0' + drive);
            if (f_stat(path, NULL) == FR_OK)
            {
                found = true;
                break;
            }
        }
        if (!found)
            goto fail;
    }

    // Splice the on-disk basename so argv[0] preserves case.
    if (!str_correct_basename(path, sizeof(path)))
        goto fail;
    DBG("pro_nfc argv[0] %s\n", path);

    if (strcmp(path, pro_running_path) == 0)
        goto already_running;

    // Full success
    bel_add(&bel_nfc_success_1);
    bel_add(&bel_nfc_success_2);
    rln_stop();
    mon_stop();
    main_stop();

    // Change to the directory containing the ROM before loading
    char *slash = NULL;
    for (char *p = path; *p; p++)
        if (str_is_sep(*p))
            slash = p;
    if (slash && slash > path)
    {
        // For a ROM in the drive root, chdir target is "DRV:/" (keep the slash);
        // for a subdir it is "DRV:/dir" (strip the last slash).
        char *term = (*(slash - 1) == ':') ? slash + 1 : slash;
        char saved = *term;
        *term = '\0';
        f_chdrive(path);
        f_chdir(path);
        *term = saved;
    }

    printf("\nNFC ");
    putchar('"');
    for (const char *p = path; *p; p++)
    {
        unsigned char c = (unsigned char)*p;
        if (c == '\\' || c == '"')
            printf("\\%c", c);
        else if (c < 32 || c >= 127)
            printf("\\%03o", c);
        else
            putchar(c);
    }
    putchar('"');
    nfc_parse_text(tag_data, len, path, sizeof(path));
    args = path;
    str_parse_string(&args);
    if (*args)
    {
        putchar(' ');
        for (const char *p = args; *p; p++)
        {
            unsigned char c = (unsigned char)*p;
            putchar(c < 32 || c >= 127 ? '?' : c);
        }
    }
    putchar('\n');
    rom_mon_load(path);
    return;

already_running:
    // Already running this ROM; beep once and bail.
    bel_add(&bel_nfc_success_1);
    return;

fail:
    bel_add(&bel_nfc_fail);
}
