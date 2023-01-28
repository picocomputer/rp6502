/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "hlp.h"
#include "lfs.h"
#include "rom.h"
#include "str.h"

static const char __in_flash("helptext") hlp_text_help[] =
    "Commands:\n"
    "HELP (command|rom)  - This help or expanded help for command or rom.\n"
    "HELP ABOUT|SYSTEM   - About includes credits. System for general usage.\n"
    "STATUS              - Show all settings and USB devices.\n"
    "CAPS (0|1|2)        - Invert or force caps while 6502 is running.\n"
    "PHI2 (kHz)          - Query or set PHI2 speed. This is the 6502 clock.\n"
    "RESB (ms)           - Query or set RESB hold time. Set to 0 for auto.\n"
    "LS (DIR|DRIVE)      - List contents of directory.\n"
    "CD (DIR)            - Change or show current directory.\n"
    "0:                  - 1:-8: Change current USB drive.\n"
    "LOAD file           - Load ROM file. Start if contains reset vector.\n"
    "INFO file           - Show help text, if any, contained in ROM file.\n"
    "INSTALL file        - Install ROM file on RIA.\n"
    "REMOVE rom          - Remove ROM from RIA.\n"
    "BOOT (rom|-)        - Select ROM to boot from cold start. \"-\" for none.\n"
    "REBOOT              - Cold start. Load and start selected boot ROM.\n"
    "RESET               - Start 6502 at current reset vector ($FFFC).\n"
    "rom                 - Load and start an installed ROM.\n"
    "UPLOAD file         - Write file. Binary chunks follow.\n"
    "UNLINK file         - Delete file.\n"
    "BINARY addr len crc - Write memory. Binary data follows.\n"
    "0000 (00 00 ...)    - Read or write memory.\n";

static const char __in_flash("helptext") hlp_text_about[] =
    "//TODO Credits will go here.";

static const char __in_flash("helptext") hlp_text_system[] =
    "This is the RIA monitor of the Picocomputer, not an operating system CLI.\n"
    "The Picocomputer does not use a traditional parallel ROM like a 27C64 or\n"
    "similar. Instead, this monitor is used to prepare the 6502 RAM with software\n"
    "that would normally be on a ROM chip. //TODO more to come.";

static const char __in_flash("helptext") hlp_text_status[] =
    "STATUS will list all configurable settings and some system information\n"
    "including a list of USB devices and their ID. The USB ID is also the drive\n"
    "number for mass storage devices (MSC). Up to 8 devices are supported.";

static const char __in_flash("helptext") hlp_text_caps[] =
    "CAPS is intended for software that doesn't recognize lower case, like many\n"
    "versions of BASIC. This is only in effect while 6502 software is running.\n"
    "It will translate both USB keyboards and the serial console. Setting is\n"
    "saved on the RIA flash.\n"
    "  0 = Normal.\n"
    "  1 = Inverted. Uppercase is lowered. Lowercase is uppered.\n"
    "  2 = Forced. Lowercase is uppered. Everything is uppercase always.";

static const char __in_flash("helptext") hlp_text_phi2[] =
    "PHI2 is the 6502 clock speed in kHz. The valid range is 1-8000 but not all\n"
    "frequencies are available. In that case, the next highest frequency will\n"
    "be automatically calculated and selected. Faster than the default speed of\n"
    "4000 kHz (4 MHz) requires \"Advanced CMOS\" glue logic (74ACxx) as well as\n"
    "overclocking of the Pi Pico RIA, which is handled automatically. The Pi Pico\n"
    "RIA will run at 240MHz for 8MHz, which is much lower than the 400+ MHz that\n"
    "it is capable of. Setting is saved on the RIA flash.";

static const char __in_flash("helptext") hlp_text_resb[] =
    "RESB is the minimum time in milliseconds that the RESB line is held low for\n"
    "a 6502 reset. This should be left on auto (0) unless you have installed extra\n"
    "hardware that needs more than 2 cycles of PHI2 while in reset. The range is\n"
    "0-255ms but beware that large delays will affect loading ROMs. Setting is\n"
    "saved on the RIA flash.";

static const char __in_flash("helptext") hlp_text_dir[] =
    "LS (also aliased as DIR) and CD are used to navigate USB mass storage\n"
    "devices. You can change to a different USB device with 1: to 8:. Use the\n"
    "STATUS command to get a list of mounted drives.";

static const char __in_flash("helptext") hlp_text_load[] =
    "LOAD and INFO read ROM files from a USB drive. A ROM file contains both\n"
    "ASCII information for the user, and binary information for the RP6502.\n"
    "Lines may end with either LF or CRLF. The first line must be:\n"
    "#!RP6502\n"
    "This is followed by HELP/INFO lines that begin with a # and a space:\n"
    "# Cool Game V0.0 by Awesome Dev\n"
    "After the info lines, binary data is prefixed with ASCII lines containing\n"
    "hex or decimal numbers indicating the address, length, and CRC-32.\n"
    "$C000 1024 0x0C0FFEE0\n"
    "This is followed by the binary data. The maximum length is 1024 bytes, so\n"
    "repeat as necessary. The CRC-32 is calculated using the same method as zip.\n"
    "If the ROM file contains data for the reset vector $FFFC-$FFFD then the\n"
    "6502 will be reset (started) immediately after loading.";

static const char __in_flash("helptext") hlp_text_install[] =
    "INSTALL and REMOVE manage the ROMs installed in the Pi Pico RIA flash memory.\n"
    "ROM files must contain a reset vector to be installed. A list of installed\n"
    "ROMs is shown on the base HELP screen. Once installed, these ROMs become an\n"
    "integrated part of the system and can be loaded manually by simply using their\n"
    "name like any other command. The ROM name must not conflict with any other\n"
    "system command and may only contain up to 16 ASCII letters. If the file\n"
    "contains an extension, it must be \".rp6502\", which will be stripped upon\n"
    "install.";

static const char __in_flash("helptext") hlp_text_boot[] =
    "BOOT selects an installed ROM to be automatically loaded and started when the\n"
    "system is power up or rebooted. For example, you might want the system to\n"
    "immediately boot into BASIC or an operating system CLI. This is used to\n"
    "provide the instant-on experience of classic 8-bit computers. Using \"-\" for\n"
    "the argument will have the system boot into the monitor you are using now.\n"
    "Setting is saved on the RIA flash.";

static struct
{
    size_t cmd_len;
    const char *const cmd;
    const char *const text;
} const COMMANDS[] = {
    {6, "status", hlp_text_status},
    {5, "about", hlp_text_about},
    {6, "system", hlp_text_system},
    {1, "0", hlp_text_system},
    {4, "0000", hlp_text_system},
    {4, "caps", hlp_text_caps},
    {4, "phi2", hlp_text_phi2},
    {4, "resb", hlp_text_resb},
    {2, "ls", hlp_text_dir},
    {3, "dir", hlp_text_dir},
    {2, "cd", hlp_text_dir},
    {2, "0:", hlp_text_dir},
    {2, "1:", hlp_text_dir},
    {2, "2:", hlp_text_dir},
    {2, "3:", hlp_text_dir},
    {2, "4:", hlp_text_dir},
    {2, "5:", hlp_text_dir},
    {2, "6:", hlp_text_dir},
    {2, "7:", hlp_text_dir},
    {2, "8:", hlp_text_dir},
    {2, "9:", hlp_text_dir},
    {4, "load", hlp_text_load},
    {4, "info", hlp_text_load},
    {7, "install", hlp_text_install},
    {6, "remove", hlp_text_install},
    {4, "boot", hlp_text_boot},

};
static const size_t COMMANDS_COUNT = sizeof COMMANDS / sizeof *COMMANDS;

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
        count = 0;
    }
    return count;
}

static void hlp_help(const char *args, size_t len)
{
    (void)(args);
    (void)(len);
    puts(hlp_text_help);
    uint32_t rom_count = hlp_roms_list(0);
    if (rom_count)
    {
        printf("%ld installed ROM%s:\n", rom_count, rom_count == 1 ? "" : "s");
        hlp_roms_list(79);
    }
    else
        printf("No installed ROMs.\n");
}

void hlp_dispatch(const char *args, size_t len)
{
    if (!len)
        return hlp_help(args, len);
    while (len && args[len - 1] == ' ')
        len--;
    for (size_t i = 0; i < COMMANDS_COUNT; i++)
    {
        if (len == COMMANDS[i].cmd_len)
            if (!strnicmp(args, COMMANDS[i].cmd, len))
            {
                puts(COMMANDS[i].text);
                return;
            }
    }
    if (!rom_help_lfs(args, len))
        puts("?No help found.");
}
