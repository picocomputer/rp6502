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
// One settings per line. e.g.
// V0
// S0=1

// The number is for multiple profiles (ATZ?/AT&W?) (not implemented yet)
static const char __in_flash("net_nvr") filename[] = "MODEM0.SYS";

static void nvr_factory_reset(nvr_settings_t *settings)
{
    settings->verbose = 0;
    settings->auto_answer = 0;
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
                               "V%d\n"
                               "S0=%d\n"
                               "",
                               settings->verbose,
                               settings->auto_answer);
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
