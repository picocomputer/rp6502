/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "str.h"
#include "mon/hlp.h"
#include "mon/rom.h"
#include "mon/vip.h"
#include "sys/lfs.h"

static const char __in_flash("helptext") hlp_text_help[] =
    "Commands:\n"
    "HELP (command|rom)  - This help or expanded help for command or rom.\n"
    "HELP ABOUT|SYSTEM   - About includes credits. System for general usage.\n"
    "STATUS              - Show hardware status and USB devices.\n"
    "SET (attr) (value)  - Change or show settings.\n"
    "LS (dir|drive)      - List contents of directory.\n"
    "CD (dir)            - Change or show current directory.\n"
    "MKDIR dir           - Make a new directory.\n"
    "0:                  - 1:-8: Change current USB drive.\n"
    "LOAD file           - Load ROM file. Start if contains reset vector.\n"
    "INFO file           - Show help text, if any, contained in ROM file.\n"
    "INSTALL file        - Install ROM file on RIA.\n"
    "REMOVE rom          - Remove ROM from RIA.\n"
    "REBOOT              - Cold start. Load and start selected boot ROM.\n"
    "RESET               - Start 6502 at current reset vector ($FFFC).\n"
    "rom                 - Load and start an installed ROM.\n"
    "UPLOAD file         - Write file. Binary chunks follow.\n"
    "UNLINK file|dir     - Delete file or empty directory.\n"
    "BINARY addr len crc - Write memory. Binary data follows.\n"
    "0000 (00 00 ...)    - Read or write memory.";

static const char __in_flash("helptext") hlp_text_set[] =
    "Settings:\n"
    "HELP SET attr       - Show information about a setting.\n"
    "SET CAPS (0|1|2)    - Invert or force caps while 6502 is running.\n"
    "SET PHI2 (kHz)      - Query or set PHI2 speed. This is the 6502 clock.\n"
    "SET RESB (ms)       - Query or set RESB hold time. Set to 0 for auto.\n"
    "SET BOOT (rom|-)    - Select ROM to boot from cold start. \"-\" for none.\n"
    "SET CP (cp)         - Query or set code page.\n"
    "SET VGA (0|1|2)     - Query or set display type for VGA output.";

static const char __in_flash("helptext") hlp_text_about[] =
    "Picocomputer 6502 - Copyright (c) 2023 Rumbledethumps.\n"
    "     Pi Pico SDKs - Copyright (c) 2020 Raspberry Pi (Trading) Ltd.\n"
    "      Tiny printf - Copyright (c) 2014-2019 Marco Paland, PALANDesign.\n"
    "          TinyUSB - Copyright (c) 2018 hathach (tinyusb.org)\n"
    "            FatFs - Copyright (c) 20xx ChaN.\n"
    "         littlefs - Copyright (c) 2022 The littlefs authors.\n"
    "                    Copyright (c) 2017 Arm Limited.";

static const char __in_flash("helptext") hlp_text_system[] =
    "The Picocomputer does not use a traditional parallel ROM like a 27C64 or\n"
    "similar. Instead, this monitor is used to prepare the 6502 RAM with software\n"
    "that would normally be on a ROM chip. The 6502 is currently in-reset right\n"
    "now; the RESB line is low. What you are seeing is coming from the Pi Pico RIA.\n"
    "You can return to this monitor at any time by pressing CTRL-ALT-DEL or sending\n"
    "a break to the console. Since these signals are handled by the Pi Pico RIA,\n"
    "they will always stop the 6502. The Pi Pico RIA does not have direct access to\n"
    "the 6502 RAM. Sourcing dual port RAM is unreliable, so a different approach is\n"
    "used. To read or write RAM, the 6502 is briefly started and a program is run in\n"
    "$FFF0-$FFF9. Many of the commands use this technique. The most basic is\n"
    "accessed by typing a hex address as a command:\n"
    "]0200\n"
    "0200 DA DA DA DA DA DA DA DA DA DA DA DA DA DA DA DA\n"
    "The 64KB of extended memory (XRAM) is mapped from $10000 to $1FFFF.\n"
    "You can also set memory. For example, to set the reset vector:\n"
    "]FFFC 00 02\n"
    "This is useful for some light debugging, but the real power is from the other\n"
    "commands you can explore with this help system. Have fun!";

static const char __in_flash("helptext") hlp_text_dir[] =
    "LS (also aliased as DIR) and CD are used to navigate USB mass storage\n"
    "devices. You can change to a different USB device with 1: to 8:. Use the\n"
    "STATUS command to get a list of mounted drives.";

static const char __in_flash("helptext") hlp_text_mkdir[] =
    "MKDIR is used to create new directories. Use UNLINK to remove empty directories.";

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

static const char __in_flash("helptext") hlp_text_reboot[] =
    "REBOOT will restart the Pi Pico RIA. It does the same thing as pressing a\n"
    "reset button attached to the Pi Pico or interrupting the power supply.";

static const char __in_flash("helptext") hlp_text_reset[] =
    "RESET will restart the 6502 by bringing RESB high. This is mainly used for\n"
    "automated testing by a script on another system connected to the console.\n"
    "For example, a build script can compile a program, upload it directly to\n"
    "6502 RAM, start it with this RESET, then optionally continue to send and\n"
    "receive data to ensure proper operation of the program.";

static const char __in_flash("helptext") hlp_text_upload[] =
    "UPLOAD is used to send a file from another system using the console port.\n"
    "The file may be any type with any name and will overwrite an existing file\n"
    "of the same name. For example, you can send a ROM file along with other\n"
    "files containing graphics or level data for a game. Then you can LOAD the\n"
    "game and test it. Think \"XMODEM/YMODEM\" but easier to implement with modern\n"
    "scripting languages. The upload is initiated with the BINARY command\n"
    "specifying the file name.\n"
    "]BINARY filename.bin\n"
    "The system will respond with a \"}\" prompt or an error message starting with\n"
    "a \"?\". Any error will abort the upload and return you to the monitor.\n"
    "There is no retry as this is not intended to be used on lossy connections.\n"
    "Specify each chunk with a length, up to 1024 bytes, and CRC-32 which you can\n"
    "compute from any zip library.\n"
    "}$400 $0C0FFEE0\n"
    "Send the binary data and you will get another \"}\" prompt or \"?\" error.\n"
    "The transfer is completed with the END command or a blank line. Your choice.\n"
    "}END\n"
    "You will return to a \"]\" prompt on success or \"?\" error on failure.";

static const char __in_flash("helptext") hlp_text_unlink[] =
    "UNLINK removes a file. Its intended use is for scripting on another system\n"
    "connected to the console. For example, you might want to delete save data\n"
    "as part of automated testing. You'll probably use this once manually after\n"
    "attempting to use the UPLOAD command from a keyboard. ;)";

static const char __in_flash("helptext") hlp_text_binary[] =
    "BINARY is the fastest way to get code or data from your build system to the\n"
    "6502 RAM. Use the command \"BINARY addr len crc\" with a maximum length of 1024\n"
    "bytes and the CRC-32 calculated with a zip library. Then send the binary.\n"
    "You will return to a \"]\" prompt on success or \"?\" error on failure.";

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

static const char __in_flash("helptext") hlp_text_boot[] =
    "BOOT selects an installed ROM to be automatically loaded and started when the\n"
    "system is power up or rebooted. For example, you might want the system to\n"
    "immediately boot into BASIC or an operating system CLI. This is used to\n"
    "provide the instant-on experience of classic 8-bit computers. Using \"-\" for\n"
    "the argument will have the system boot into the monitor you are using now.\n"
    "Setting is saved on the RIA flash.";

static const char __in_flash("helptext") hlp_text_code_page[] =
    "SET CP selects a code page for system text. The following is supported:\n"
    "437, 720, 737, 771, 775, 850, 852, 855, 857, 860, 861, 862, 863, 864, 865,\n"
    "866, 869, 932, 936, 949, 950.  Code pages 720, 932, 936, 949, 950 do not have\n"
    "VGA fonts."
#if RP6502_CODE_PAGE
#define xstr(s) str(s)
#define str(s) #s
    "\nThis is a development build. Only " xstr(RP6502_CODE_PAGE) " is available.";
#else
    "";
#endif

static const char __in_flash("helptext") hlp_text_vga[] =
    "SET VGA selects the display type for VGA output. All canvas resolutions are\n"
    "supported by all display types. Display type is used to maintain square\n"
    "pixels and minimize letterboxing. Note that 1280x1024 is 5:4 so 4:3 graphics\n"
    "will be letterboxed slightly but you'll get 2 extra rows on the terminal.\n"
    "  0 - 640x480\n"
    "  1 - 640x480 and 1280x720, 16:9 modes will not letterbox\n"
    "  2 - 1280x1024, all graphics modes will letterbox";

static struct
{
    size_t cmd_len;
    const char *const cmd;
    const char *const text;
} const COMMANDS[] = {
    {3, "set", hlp_text_set}, // must be first
    {6, "status", hlp_text_status},
    {5, "about", hlp_text_about},
    {7, "credits", hlp_text_about},
    {6, "system", hlp_text_system},
    {1, "0", hlp_text_system},
    {4, "0000", hlp_text_system},
    {2, "ls", hlp_text_dir},
    {3, "dir", hlp_text_dir},
    {2, "cd", hlp_text_dir},
    {5, "chdir", hlp_text_dir},
    {5, "mkdir", hlp_text_mkdir},
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
    {6, "reboot", hlp_text_reboot},
    {5, "reset", hlp_text_reset},
    {6, "upload", hlp_text_upload},
    {6, "unlink", hlp_text_unlink},
    {6, "binary", hlp_text_binary},
};
static const size_t COMMANDS_COUNT = sizeof COMMANDS / sizeof *COMMANDS;

static struct
{
    size_t set_len;
    const char *const cmd;
    const char *const text;
} const SETTINGS[] = {
    {4, "caps", hlp_text_caps},
    {4, "phi2", hlp_text_phi2},
    {4, "resb", hlp_text_resb},
    {4, "boot", hlp_text_boot},
    {2, "cp", hlp_text_code_page},
    {3, "vga", hlp_text_vga},
};
static const size_t SETTINGS_COUNT = sizeof SETTINGS / sizeof *SETTINGS;

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
            if (!(i && ch >= '0' && ch <= '9') && (ch < 'A' || ch > 'Z'))
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

// Returns NULL if not found.
const char *help_text_lookup(const char *args, size_t len)
{
    size_t cmd_len;
    for (cmd_len = 0; cmd_len < len; cmd_len++)
        if (args[cmd_len] == ' ')
            break;
    // SET command has another level of help
    if (cmd_len == COMMANDS[0].cmd_len && !strnicmp(args, COMMANDS[0].cmd, cmd_len))
    {
        args += cmd_len;
        len -= cmd_len;
        while (len && args[0] == ' ')
            args++, len--;
        size_t set_len;
        for (set_len = 0; set_len < len; set_len++)
            if (args[set_len] == ' ')
                break;
        if (!set_len)
            return COMMANDS[0].text;
        for (size_t i = 0; i < SETTINGS_COUNT; i++)
            if (set_len == SETTINGS[i].set_len)
                if (!strnicmp(args, SETTINGS[i].cmd, set_len))
                    return SETTINGS[i].text;
        return NULL;
    }
    // Help for commands and a couple special words.
    for (size_t i = 1; i < COMMANDS_COUNT; i++)
        if (cmd_len == COMMANDS[i].cmd_len)
            if (!strnicmp(args, COMMANDS[i].cmd, cmd_len))
                return COMMANDS[i].text;
    return NULL;
}

void hlp_mon_help(const char *args, size_t len)
{
    if (!len)
        return hlp_help(args, len);
    while (len && args[len - 1] == ' ')
        len--;
    const char *text = help_text_lookup(args, len);
    if (text)
    {
        puts(text);
        if (text == hlp_text_about)
            vip_print();
    }
    else
    {
        if (!rom_help(args, len))
            puts("?No help found.");
    }
}
