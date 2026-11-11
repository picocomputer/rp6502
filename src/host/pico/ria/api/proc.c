/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/api/api.h"
#include "core/api/arg.h"
#include "ria/api/proc.h"
#include "core/aud/bel.h"
#include "core/com/com.h"
#include "core/sys/debug_log.h"
#include "core/sys/sys.h"
#include "ria/mon/mon.h"
#include "ria/mon/rom.h"
#include "core/com/rln.h"
#include "core/str/oem.h"
#include "core/str/path.h"
#include "core/str/str.h"
#include "osal/dir.h"
#include "sys/path.h"
#include "ria/usb/ndef.h"
#include "ria/usb/nfc.h"
#include <fatfs/ff.h>
#include <stdio.h>

void proc_exec_start(void)
{
    sys_stop();
    if (!rom_exec())
        proc_exec_failed();
}

void proc_exec_relaunch(void)
{
    if (!rom_exec())
        proc_exec_failed();
}

bool proc_exec_inflight(void)
{
    return rom_active();
}

/* The card names the running ROM when its path names the same file and its
 * arguments are the same strings. A card for the same ROM with other
 * arguments starts it again. This parses its own copy of the arguments, so
 * it must come after proc_nfc_find is done with first_arg, which shares the
 * parse's buffer. */
static bool proc_nfc_running(const char *path, const char *args)
{
    if (!path_eq(path, proc_running()))
        return false;
    for (uint16_t i = 1;; i++)
    {
        const char *arg = str_parse_string(&args);
        const char *run = arg_index(i);
        if (!arg || !run)
            return !arg && !run && str_parse_end(args);
        if (strcmp(arg, run))
            return false;
    }
}

/* Resolves the ROM a card's text names into path, and leaves *args at the
 * text after it. A path with no drive is looked for on each drive in turn.
 * The caller puts FatFs on the code page the ROM will run under. */
static bool proc_nfc_find(const char *text, char *path, size_t path_size,
                          const char **args)
{
    *args = text;
    const char *first_arg = str_parse_string(args);
    if (!first_arg)
        return false;

    if (*first_arg == ':')
    {
        if (!rom_installed(first_arg + 1, path))
            return false;
    }
    else if (strchr(first_arg, ':'))
    {
        // NFC paths ignore the CWD: imply the leading '/' after the drive.
        const char *colon = strchr(first_arg, ':');
        const char *rest = colon + 1;
        if (path_is_sep(*rest))
            rest++;
        snprintf(path, path_size, "%.*s/%s",
                 (int)(colon + 1 - first_arg), first_arg, rest);
        const char *abs = path_abs(path);
        if (!abs)
            return false;
        strncpy(path, abs, path_size - 1);
        path[path_size - 1] = '\0';
        if (f_stat(path, NULL) != FR_OK)
            return false;
    }
    else
    {
        // Build canonical "MSC0:/path"
        const char *p = (*first_arg == '/') ? first_arg + 1 : first_arg;
        snprintf(path, path_size, "MSC0:/%s", p);
        const char *abs = path_abs(path);
        if (!abs)
            return false;
        strncpy(path, abs, path_size - 1);
        path[path_size - 1] = '\0';
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
            return false;
    }

    return true;
}

void proc_nfc(const uint8_t *tag_data, size_t len)
{
    char text[256];
    char path[256];
    const char *args;
    RP6502_LOG(proc, DEBUG, "nfc %zu bytes", len);

    if (mon_busy())
        goto fail;
    /* The ROM starts under the configured code page, which the stop below
     * restores if a running program chose another, so the card's text is
     * converted to it and the lookup is made under it. */
    const uint16_t page = oem_get_code_page_restore();
    if (!ndef_parse_text(tag_data, len, page, text, sizeof(text)))
        goto fail;
    RP6502_LOG(proc, DEBUG, "nfc text %s", text);
    oem_fs_code_page(page);
    bool found = proc_nfc_find(text, path, sizeof(path), &args);
    oem_fs_code_page(oem_get_code_page_run());
    if (!found)
        goto fail;

    RP6502_LOG(proc, DEBUG, "nfc argv[0] %s", path);

    if (proc_nfc_running(path, args))
        goto already_running;

    // Full success. The tag's ROM starts as if typed at the monitor, so a
    // running program ends with its chain, and a monitor read is cancelled.
    bel_add(&bel_nfc_success_1);
    bel_add(&bel_nfc_success_2);
    proc_cancel_launcher();
    sys_stop_now();
    rln_stop();
    mon_stop();
    com_clean_line();

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

    printf("NFC ");
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
    rom_load_argv(path, args);
    return;

already_running:
    // Already running this ROM; beep once and bail.
    bel_add(&bel_nfc_success_1);
    return;

fail:
    bel_add(&bel_nfc_fail);
}
