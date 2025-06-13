/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "str.h"
#include "net/nvr.h"
#include "sys/lfs.h"
#include "sys/mem.h"

#if defined(DEBUG_RIA_NET) || defined(DEBUG_RIA_NET_NVR)
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__);
#else
#define DBG(...) \
    {            \
    }
#endif

// Configuration is a plain ASCII file on the LFS.
// One setting per line. e.g. "E1\nV1\nS0=0\n"
static const char __in_flash("net_nvr") filename[] = "MODEM0.SYS";

static void nvr_factory_reset(nvr_settings_t *settings)
{
    settings->echo = 1;        // E1
    settings->verbose = 1;     // V1
    settings->auto_answer = 0; // S0=0
    settings->escChar = '+';   // S2=43
    settings->crChar = '\r';   // S3=13
    settings->lfChar = '\n';   // S4=10
    settings->bsChar = '\b';   // S5=8
}

bool nvr_write(const nvr_settings_t *settings)
{
    lfs_file_t lfs_file;
    LFS_FILE_CONFIG(lfs_file_config);
    int lfsresult = lfs_file_opencfg(&lfs_volume, &lfs_file, filename,
                                     LFS_O_RDWR | LFS_O_CREAT,
                                     &lfs_file_config);
    if (lfsresult < 0)
        DBG("?Unable to lfs_file_opencfg %s for writing (%d)\n", filename, lfsresult);
    if (lfsresult >= 0)
        if ((lfsresult = lfs_file_truncate(&lfs_volume, &lfs_file, 0)) < 0)
            DBG("?Unable to lfs_file_truncate %s (%d)\n", filename, lfsresult);
    if (lfsresult >= 0)
    {
        lfsresult = lfs_printf(&lfs_volume, &lfs_file,
                               "E%u\n"
                               "V%u\n"
                               "S0=%u\n"
                               "S2=%u\n"
                               "S3=%u\n"
                               "S4=%u\n"
                               "S5=%u\n"
                               "",
                               settings->echo,
                               settings->verbose,
                               settings->auto_answer,
                               settings->escChar,
                               settings->crChar,
                               settings->lfChar,
                               settings->bsChar);
        if (lfsresult < 0)
            DBG("?Unable to write %s contents (%d)\n", filename, lfsresult);
    }
    int lfscloseresult = lfs_file_close(&lfs_volume, &lfs_file);
    if (lfscloseresult < 0)
        DBG("?Unable to lfs_file_close %s (%d)\n", filename, lfscloseresult);
    if (lfsresult < 0 || lfscloseresult < 0)
    {
        lfs_remove(&lfs_volume, filename);
        return false;
    }
    return true;
}

bool nvr_read(nvr_settings_t *settings)
{
    nvr_factory_reset(settings);
    lfs_file_t lfs_file;
    LFS_FILE_CONFIG(lfs_file_config);
    int lfsresult = lfs_file_opencfg(&lfs_volume, &lfs_file, filename,
                                     LFS_O_RDONLY, &lfs_file_config);
    mbuf[0] = 0;
    if (lfsresult < 0)
    {
        if (lfsresult == LFS_ERR_NOENT)
            return true;
        DBG("?Unable to lfs_file_opencfg %s for reading (%d)\n", filename, lfsresult);
        return false;
    }
    while (lfs_gets((char *)mbuf, MBUF_SIZE, &lfs_volume, &lfs_file))
    {
        size_t len = strlen((char *)mbuf);
        while (len && mbuf[len - 1] == '\n')
            len--;
        mbuf[len] = 0;
        const char *str = (char *)mbuf + 1;
        len -= 1;
        switch (mbuf[0])
        {
        case 'E':
            parse_uint8(&str, &len, &settings->echo);
            break;
        case 'V':
            parse_uint8(&str, &len, &settings->verbose);
            break;
        case 'S':
            uint8_t s_register;
            parse_uint8(&str, &len, &s_register);
            if (str[0] != '=')
                break;
            const char *str = (char *)mbuf + 1;
            len -= 1;
            switch (s_register)
            {
            case 0:
                parse_uint8(&str, &len, &settings->auto_answer);
                break;
            case 2:
                parse_uint8(&str, &len, &settings->escChar);
                break;
            case 3:
                parse_uint8(&str, &len, &settings->crChar);
                break;
            case 4:
                parse_uint8(&str, &len, &settings->lfChar);
                break;
            case 5:
                parse_uint8(&str, &len, &settings->bsChar);
                break;
            default:
                break;
            }
            break;
        default:
            break;
        }
    }
    lfsresult = lfs_file_close(&lfs_volume, &lfs_file);
    if (lfsresult < 0)
    {
        DBG("?Unable to lfs_file_close %s (%d)\n", filename, lfsresult);
        return false;
    }
    return true;
}
