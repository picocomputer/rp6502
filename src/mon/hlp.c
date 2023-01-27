/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "hlp.h"
#include "lfs.h"

// Use width=0 to supress printing. Returns count.
// Anything with only uppercase letters is counted.
static uint32_t hlp_roms_list(uint32_t width)
{
    uint32_t count = 0;
    uint32_t col = 0;
    lfs_dir_t lfs_dir;
    struct lfs_info lfs_info;
    int result = lfs_dir_open(&lfs_volume, &lfs_dir, "");
    if (result < 0)
    {
        printf("?Unable to open ROMs directory (%d)\n", result);
        return 0;
    }
    while (true)
    {
        result = lfs_dir_read(&lfs_volume, &lfs_dir, &lfs_info);
        if (!result)
            break;
        if (result < 0)
        {
            printf("?Error reading ROMs directory (%d)\n", result);
            count = 0;
            break;
        }
        bool is_ok = true;
        size_t len = strlen(lfs_info.name);
        for (size_t i = 0; i < len; i++)
        {
            char ch = lfs_info.name[i];
            if (ch < 'A' || ch > 'Z')
                is_ok = false;
        }
        if (is_ok && width)
        {
            if (count)
            {
                putchar(',');
                col += 1;
            }
            if (col + len > width - 2)
            {
                printf("\n%s", lfs_info.name);
                col = len;
            }
            else
            {
                if (col)
                {
                    putchar(' ');
                    col += 1;
                }
                printf("%s", lfs_info.name);
                col += len;
            }
        }
        if (is_ok)
            count++;
    }
    if (width)
    {
        if (count)
        {
            putchar('.');
            col++;
        }
        putchar('\n');
    }
    result = lfs_dir_close(&lfs_volume, &lfs_dir);
    if (result < 0)
    {
        printf("?Error closing ROMs directory (%d)\n", result);
    }
    return count;
}

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
        "BOOT (rom|-)        - Select ROM to boot from cold start. \"-\" for none.\n"
        "REBOOT              - Cold start. Load and start selected boot ROM.\n"
        "rom                 - Load and start an installed ROM.\n"
        "UPLOAD file         - Write file. Binary chunks follow.\n"
        "UNLINK file         - Delete file.\n"
        "RESET               - Start 6502 at current reset vector ($FFFC).\n"
        "BINARY addr len crc - Write memory. Binary data follows.\n"
        "0000 00 00 ...      - Write memory.\n"
        "0000                - Read memory.";
    puts(cmdhelp);

    uint32_t rom_count = hlp_roms_list(0);
    if (rom_count)
    {
        printf("%ld installed ROM%s:\n", rom_count, rom_count == 1 ? "" : "s");
        hlp_roms_list(79);
    }
    else
        printf("No installed ROMs.\n");
}
