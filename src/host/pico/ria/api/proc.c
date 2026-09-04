/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/api/api.h"
#include "core/api/arg.h"
#include "ria/api/proc.h"
#include "core/aud/bel.h"
#include "core/sys/sys.h"
#include "ria/mon/mon.h"
#include "ria/mon/rom.h"
#include "core/str/rln.h"
#include "core/str/path.h"
#include "core/str/str.h"
#include "sys/path.h"
#include "ria/usb/nfc.h"
#include <fatfs/ff.h>
#include <stdio.h>

#if defined(DEBUG_API) || defined(DEBUG_API_PROC)
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

/* This machine loads through a task-driven state machine; both of these are
 * rom_exec picking up the argv the caller has already set. Op 0x09 stops the
 * program first -- the relaunch is running inside a stop already. */
void proc_exec_start(void)
{
    sys_stop();
    rom_exec();
}

void proc_exec_relaunch(void)
{
    rom_exec();
}

/* A load already committed by proc_api_exec or proc_nfc must not be clobbered
 * by the launcher. */
bool proc_exec_inflight(void)
{
    return rom_active();
}

void proc_nfc(const uint8_t *tag_data, size_t len)
{
    char path[256];
    DBG("proc_nfc(%zu bytes)\n", len);

    if (!nfc_parse_text(tag_data, len, path, sizeof(path)))
        goto fail;
    DBG("proc_nfc text %s\n", path);

    const char *args = path;
    const char *first_arg = str_parse_string(&args);
    if (!first_arg)
        goto fail;
    if (*first_arg == ':')
    {
        /* An installed name: no drive to scan, no cwd to move, no case to
         * correct. The open answers; a miss fails like any bad path. */
        rom_load_argv(first_arg, args);
        return;
    }

    bool has_drive = (strchr(first_arg, ':') != NULL);
    if (has_drive)
    {
        // NFC paths ignore the CWD: imply the leading '/' after the drive.
        const char *colon = strchr(first_arg, ':');
        const char *rest = colon + 1;
        if (path_is_sep(*rest))
            rest++;
        snprintf(path, sizeof(path), "%.*s/%s",
                 (int)(colon + 1 - first_arg), first_arg, rest);
        const char *abs = path_abs(path);
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
        const char *abs = path_abs(path);
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
    if (!path_correct_basename(path, sizeof(path)))
        goto fail;
    DBG("proc_nfc argv[0] %s\n", path);

    if (strcmp(path, proc_running()) == 0)
        goto already_running;

    // Full success
    bel_add(&bel_nfc_success_1);
    bel_add(&bel_nfc_success_2);
    sys_stop(); /* the walk behind it stops rln and mon */

    // Change to the directory containing the ROM before loading
    char *slash = NULL;
    for (char *p = path; *p; p++)
        if (path_is_sep(*p))
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
