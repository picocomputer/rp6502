/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "hlp.h"
#include "lfs.h"

void hlp_help(const char *args, size_t len)
{
    (void)(args);
    (void)(len);
    static const char *__in_flash("cmdhelp") cmdhelp =
        "Commands:\n"
        "HELP (COMMAND)      - This help or expanded help for command.\n"
        "STATUS              - Show all settings and USB devices.\n"
        "CAPS (0|1|2)        - Invert or force caps while 6502 is running.\n"
        "PHI2 (kHz)          - Query or set PHI2 speed. This is the 6502 clock.\n"
        "RESB (ms)           - Query or set RESB hold time. Set to 0 for auto.\n"
        "LS (DIR|DRIVE)      - List contents of directory.\n"
        "CD (DIR)            - Change or show current directory.\n"
        "0:                  - 1:-8: Change current USB drive.\n"
        "LOAD file           - Load ROM file. Start if contains reset vector.\n"
        "INSTALL file        - Install ROM file on RIA.\n"
        "REMOVE rom          - Remove ROM from RIA.\n"
        "BOOT rom            - Select ROM to boot from cold start.\n"
        "REBOOT              - Load and start selected boot ROM.\n"
        "rom                 - Load and start an installed ROM.\n"
        "UPLOAD file         - Write file. Binary chunks follow.\n"
        "RESET               - Start 6502 at current reset vector ($FFFC).\n"
        "BINARY addr len crc - Write memory. Binary data follows.\n"
        "0000 00 00 ...      - Write memory.\n"
        "0000                - Read memory.";
    puts(cmdhelp);

    lfs_dir_t lfs_dir;
    struct lfs_info lfs_info;

    int result = lfs_dir_open(&lfs_volume, &lfs_dir, "");
    if (result < 0)
    {
        printf("err1 %d\n", result);
    }

    while (true)
    {
        result = lfs_dir_read(&lfs_volume, &lfs_dir, &lfs_info);
        if (!result)
            break;
        if (result < 0)
        {
            printf("?result %d\n", result);
            break;
        }
        printf("lfs: %ld %s\n", lfs_info.size, lfs_info.name);
    }

    result = lfs_dir_close(&lfs_volume, &lfs_dir);
    if (result < 0)
    {
        printf("err2 %d\n", result);
    }
}
