/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/api/api.h"
#include "core/api/arg.h"
#include "core/ria/regs.h"
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

/* One parse of a card's arguments, before anything is stopped. Every argument
 * must parse, and with argv[0] all of them must fit the ARGV data, because
 * they are built only after the stop, over the running program's, so a card
 * that cannot be built is refused while the program carries on. *running is
 * set when the card names the running ROM: the same file and the same
 * arguments. A card for the same ROM with other arguments starts it again.
 * This shares str_parse_string's buffer with proc_nfc_find's first_arg, which
 * is done with by now. */
static bool proc_nfc_args(const char *path, const char *args, bool *running)
{
    size_t size = arg_size_append(ARG_SIZE_EMPTY, path);
    bool same = path_eq(path, proc_running());
    uint16_t i = 1;
    const char *arg;
    while ((arg = str_parse_string(&args)) != NULL)
    {
        size = arg_size_append(size, arg);
        if (size > XSTACK_SIZE)
            return false;
        if (same)
        {
            const char *run = arg_index(i++);
            same = run && !strcmp(arg, run);
        }
    }
    *running = same && !arg_index(i);
    return str_parse_end(args);
}

/* A file and not a directory. f_stat tells them apart only through a FILINFO,
 * a few hundred bytes of the stack, where an f_opendir that fails for a file
 * needs a DIR of about fifty. */
static bool proc_nfc_is_file(const char *path)
{
    if (f_stat(path, NULL) != FR_OK)
        return false;
    DIR dir;
    if (f_opendir(&dir, path) != FR_OK)
        return true;
    f_closedir(&dir);
    return false;
}

/* Resolves the ROM a card's text names into path, and leaves *args at the
 * text after it. A path with no drive is looked for on each drive in turn,
 * and a directory is no ROM.
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
        if (!proc_nfc_is_file(path))
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
            if (proc_nfc_is_file(path))
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

    bool running;
    if (!proc_nfc_args(path, args, &running))
        goto fail;
    if (running)
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
