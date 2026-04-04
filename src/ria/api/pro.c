/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "api/api.h"
#include "api/pro.h"
#include "aud/bel.h"
#include "main.h"
#include "mon/mon.h"
#include "mon/rom.h"
#include "str/str.h"
#include "usb/nfc.h"
#include <fatfs/ff.h>
#include <assert.h>
#include <stdio.h>

#if defined(DEBUG_RIA_API) || defined(DEBUG_RIA_API_PRO)
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

// Records argv[0] of the currently running process.
static char pro_running[256];

// Records the launcher that will re-run when program ends.
static char pro_launcher[256];

// A zero terminated list of uint16 which points
// to zero terminated strings within pro_argv.
// Maintans no space between pointers and chars.
static uint8_t pro_argv[XSTACK_SIZE];

void pro_run(void)
{
    const char *argv0 = pro_argv_index(0);
    if (argv0)
    {
        strncpy(pro_running, argv0, sizeof(pro_running) - 1);
        pro_running[sizeof(pro_running) - 1] = '\0';
    }
    else
        pro_running[0] = '\0';
}

void pro_stop(void)
{
    if (rom_active())
    {
        // pro_api_exec or pro_nfc launching
        pro_running[0] = '\0';
        return;
    }
    bool relaunch = pro_launcher[0] != '\0' &&
                    strcmp(pro_running, pro_launcher) != 0;
    pro_running[0] = '\0';
    if (!relaunch)
        pro_launcher[0] = '\0';
    if (relaunch)
    {
        pro_argv_clear();
        pro_argv_append(pro_launcher);
        rom_exec();
    }
}

void pro_break(void)
{
    pro_launcher[0] = '\0';
}

uint16_t pro_argv_count(void)
{
    for (uint16_t i = 0; i < XSTACK_SIZE / 2; i++)
        if (pro_argv[i * 2] == 0 && pro_argv[i * 2 + 1] == 0)
            return i;
    return 0;
}

void pro_argv_clear(void)
{
    pro_argv[0] = pro_argv[1] = 0;
}

static uint16_t pro_argv_size(void)
{
    uint16_t count = pro_argv_count();
    if (count == 0)
        return 2;
    uint16_t last = (count - 1) * 2;
    uint16_t offset = pro_argv[last] | ((uint16_t)pro_argv[last + 1] << 8);
    return offset + (uint16_t)strlen((const char *)&pro_argv[offset]) + 1;
}

static bool pro_argv_validate(void)
{
    uint16_t count = pro_argv_count();
    uint16_t pos = (count + 1) * 2;
    if (pos >= XSTACK_SIZE)
        return false;
    for (uint16_t i = 0; i < count; i++)
    {
        uint16_t offset = pro_argv[i * 2] | ((uint16_t)pro_argv[i * 2 + 1] << 8);
        if (offset != pos)
            return false;
        while (pos < XSTACK_SIZE && pro_argv[pos] != 0)
            pos++;
        if (pos >= XSTACK_SIZE)
            return false;
        pos++;
    }
    return true;
}

bool pro_argv_append(const char *str)
{
    uint16_t count = pro_argv_count();
    uint16_t old_strings_start = (count + 1) * 2;
    uint16_t old_size = pro_argv_size();
    uint16_t strings_len = old_size - old_strings_start;
    uint16_t new_str_len = (uint16_t)strlen(str) + 1;
    if (old_size + 2 + new_str_len > XSTACK_SIZE)
        return false;
    memmove(&pro_argv[old_strings_start + 2], &pro_argv[old_strings_start], strings_len);
    for (uint16_t i = 0; i < count; i++)
    {
        uint16_t offset = pro_argv[i * 2] | ((uint16_t)pro_argv[i * 2 + 1] << 8);
        offset += 2;
        pro_argv[i * 2] = offset & 0xFF;
        pro_argv[i * 2 + 1] = offset >> 8;
    }
    uint16_t new_offset = old_strings_start + 2 + strings_len;
    pro_argv[count * 2] = new_offset & 0xFF;
    pro_argv[count * 2 + 1] = new_offset >> 8;
    pro_argv[(count + 1) * 2] = 0;
    pro_argv[(count + 1) * 2 + 1] = 0;
    memcpy(&pro_argv[new_offset], str, new_str_len);
    return true;
}

const char *pro_argv_index(uint16_t idx)
{
    if (idx >= pro_argv_count())
        return NULL;
    uint16_t offset = pro_argv[idx * 2] | ((uint16_t)pro_argv[idx * 2 + 1] << 8);
    return (const char *)&pro_argv[offset];
}

bool pro_argv_replace(uint16_t idx, const char *str)
{
    uint16_t count = pro_argv_count();
    if (idx >= count)
        return false;
    uint16_t old_offset = pro_argv[idx * 2] | ((uint16_t)pro_argv[idx * 2 + 1] << 8);
    uint16_t old_len = (uint16_t)strlen((const char *)&pro_argv[old_offset]) + 1;
    uint16_t new_len = (uint16_t)strlen(str) + 1;
    uint16_t old_size = pro_argv_size();
    uint16_t tail_len = old_size - (old_offset + old_len);
    if (new_len != old_len)
    {
        if (new_len > old_len && old_size + (new_len - old_len) > XSTACK_SIZE)
            return false;
        memmove(&pro_argv[old_offset + new_len],
                &pro_argv[old_offset + old_len],
                tail_len);
        for (uint16_t i = 0; i < count; i++)
        {
            uint16_t offset = pro_argv[i * 2] | ((uint16_t)pro_argv[i * 2 + 1] << 8);
            if (offset >= old_offset + old_len)
            {
                if (new_len > old_len)
                    offset += new_len - old_len;
                else
                    offset -= old_len - new_len;
                pro_argv[i * 2] = offset & 0xFF;
                pro_argv[i * 2 + 1] = offset >> 8;
            }
        }
    }
    memcpy(&pro_argv[old_offset], str, new_len);
    return true;
}

bool pro_api_argv(void)
{
    uint16_t size = pro_argv_size();
    xstack_ptr = XSTACK_SIZE - size;
    memcpy(&xstack[xstack_ptr], pro_argv, size);
    return api_return_ax(size);
}

bool pro_api_exec(void)
{
    size_t ptr = xstack_ptr;
    uint16_t size = (uint16_t)(XSTACK_SIZE - ptr);
    memcpy(pro_argv, &xstack[ptr], size);
    memset(&pro_argv[size], 0, XSTACK_SIZE - size);
    xstack_ptr = XSTACK_SIZE;
    if (!pro_argv_validate() || !pro_argv_count())
    {
        pro_argv_clear();
        return api_return_errno(API_EINVAL);
    }
    // If we get this far, always stop.
    // Problems in rom.c will log to the console
    main_stop();
    rom_exec();
    return api_return_ax(0);
}

bool pro_get_launcher(void)
{
    return pro_launcher[0] != '\0';
}

void pro_set_launcher(bool is_launcher)
{
    if (is_launcher)
    {
        strncpy(pro_launcher, pro_running, sizeof(pro_launcher) - 1);
        pro_launcher[sizeof(pro_launcher) - 1] = '\0';
    }
    else
        pro_launcher[0] = '\0';
}

void pro_nfc(const uint8_t *tag_data, size_t len)
{
    char work[256];
    DBG("pro_nfc(%zu bytes)\n", len);

    if (!nfc_parse_text(tag_data, len, work, sizeof(work)))
        goto fail;
    DBG("pro_nfc text %s\n", work);

    // Parse the first arg for the ROM path
    const char *args = work;
    const char *first_arg = str_parse_string(&args);
    if (!first_arg || *first_arg == ':')
        goto fail;

    bool has_drive = (strchr(first_arg, ':') != NULL);
    if (has_drive)
    {
        strncpy(work, first_arg, sizeof(work) - 1);
        work[sizeof(work) - 1] = '\0';
        const char *abs = str_abs_path(work);
        if (!abs)
            goto fail;
        if (strcmp(abs, pro_running) == 0)
        {
            // Half success, already running
            bel_add(&bel_nfc_success_1);
            return;
        }
        strncpy(work, abs, sizeof(work) - 1);
        work[sizeof(work) - 1] = '\0';
        DBG("pro_nfc argv[0] %s\n", work);
        FILINFO finfo;
        if (f_stat(work, &finfo) != FR_OK)
            goto fail;
    }
    else
    {
        // Build canonical "MSC0:/path"
        const char *p = (*first_arg == '/') ? first_arg + 1 : first_arg;
        snprintf(work, sizeof(work), "MSC0:/%s", p);
        const char *abs = str_abs_path(work);
        if (!abs)
            goto fail;
        strncpy(work, abs, sizeof(work) - 1);
        work[sizeof(work) - 1] = '\0';
        bool found = false;
        for (int drive = 0; drive <= 9; drive++)
        {
            work[3] = (char)('0' + drive);
            FILINFO finfo;
            if (f_stat(work, &finfo) == FR_OK)
            {
                found = true;
                break;
            }
        }
        if (!found)
            goto fail;
        if (strcmp(work, pro_running) == 0)
        {
            // Half success, already running
            bel_add(&bel_nfc_success_1);
            return;
        }
        DBG("pro_nfc argv[0] %s\n", work);
    }

    // Full success
    bel_add(&bel_nfc_success_1);
    bel_add(&bel_nfc_success_2);
    mon_stop(); // reset input
    main_stop();

    // Change to the directory containing the ROM before loading
    char *slash = strrchr(work, '/');
    if (slash && slash > work)
    {
        // For root ("DRV:/file"), keep the slash; otherwise strip it
        char *term = (*(slash - 1) == ':') ? slash + 1 : slash;
        char saved = *term;
        *term = '\0';
        f_chdrive(work);
        f_chdir(work);
        *term = saved;
    }

    printf(STR_SYS_TERM_RESET);
    printf("NFC ");
    putchar('"');
    for (const char *p = work; *p; p++)
    {
        unsigned char c = (unsigned char)*p;
        if (c == '\\' || c == '"')
            printf("\\%c", c);
        else if (c < 32 || c > 126)
            printf("\\%03o", c);
        else
            putchar(c);
    }
    putchar('"');
    nfc_parse_text(tag_data, len, work, sizeof(work));
    args = work;
    str_parse_string(&args);
    if (*args)
    {
        putchar(' ');
        for (const char *p = args; *p; p++)
        {
            unsigned char c = (unsigned char)*p;
            putchar(c < 32 || c > 127 ? '?' : c);
        }
    }
    putchar('\n');
    rom_mon_load(work);
    return;

fail:
    bel_add(&bel_nfc_fail);
}
