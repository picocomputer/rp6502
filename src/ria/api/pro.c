/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "api/api.h"
#include "api/pro.h"
#include "aud/bel.h"
#include "main.h"
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

// Records argv[0] of the most recently started process.
static char pro_running[256];

// A zero terminated list of uint16 which points
// to zero terminated strings within pro_argv.
// Maintans no space between pointers and chars.
static uint8_t pro_argv[XSTACK_SIZE];

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

void pro_run(void)
{
    assert(!main_active());
    const char *argv0 = pro_argv_index(0);
    if (argv0)
    {
        strncpy(pro_running, argv0, sizeof(pro_running) - 1);
        pro_running[sizeof(pro_running) - 1] = '\0';
    }
    else
        pro_running[0] = '\0';
    main_run();
}

void pro_nfc(const uint8_t *ndef, size_t len)
{
    char work[256];
    DBG("pro_nfc(%zu bytes)\n", len);

    if (!nfc_parse_text(ndef, len, work, sizeof(work)))
    {
        bel_add(&bel_nfc_fail);
        return;
    }
    DBG("pro_nfc text %s\n", work);

    // Parse the first arg for the ROM path, then resolve to absolute path
    const char *args = work;
    const char *first_arg = str_parse_string(&args);
    if (!first_arg || *first_arg == ':')
    {
        bel_add(&bel_nfc_fail);
        return;
    }
    // Copy first_arg into work before str_abs_path (both share the same static buffer)
    strncpy(work, first_arg, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';
    const char *abs = str_abs_path(work);
    if (!abs)
    {
        bel_add(&bel_nfc_fail);
        return;
    }
    if (strcmp(abs, pro_running) == 0)
    {
        bel_add(&bel_nfc_fail);
        return;
    }
    strncpy(work, abs, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';
    DBG("pro_nfc argv[0] %s\n", work);

    // Check file exists in FatFS
    FILINFO finfo;
    if (f_stat(work, &finfo) != FR_OK)
    {
        bel_add(&bel_nfc_fail);
        return;
    }

    bel_add(&bel_nfc_success_1);
    bel_add(&bel_nfc_success_2);
    main_stop();

    // Change to the directory containing the ROM before loading
    char *slash = strrchr(work, '/');
    if (slash && slash > work)
    {
        *slash = '\0';
        f_chdir(work);
    }
    nfc_parse_text(ndef, len, work, sizeof(work));
    DBG("pro_nfc loading %s\n", work);
    rom_mon_load(work);
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
