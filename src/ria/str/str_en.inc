/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* English */

#if !defined(X)
#define X(name, value)
#endif

X(STR_ERR_INVALID_ARGUMENT, "?invalid argument\n")
X(STR_ERR_UNKNOWN_COMMAND, "?unknown command\n")

X(STR_ON, "On")
X(STR_OFF, "Off")
X(STR_ENABLED, "Enabled")
X(STR_DISABLED, "Disabled")
X(STR_WORLDWIDE, "Worldwide")
X(STR_PARENS_NONE, "(none)")
X(STR_PARENS_SET, "(set)")
X(STR_BLE_PAIRING, ", pairing")
X(STR_BLE_NO_RF, ", no RF")

X(STR_HLP_HELP,
  "Commands:\n"
  "HELP (command|rom)  - This help or expanded help for command or rom.\n"
  "HELP ABOUT|SYSTEM   - About includes credits. System for general usage.\n"
  "STATUS              - Show status of system and connected devices.\n"
  "SET (attr) (value)  - Change or show settings.\n"
  "LS (dir|drive)      - List contents of directory.\n"
  "CD (dir)            - Change or show current directory.\n"
  "(USB)0:             - USB0:-USB7: Change current USB drive.\n"
  "LOAD file           - Load ROM file. Start if contains reset vector.\n"
  "INFO file           - Show help text, if any, contained in ROM file.\n"
  "INSTALL file        - Install ROM file on RIA.\n"
  "rom                 - Load and start an installed ROM.\n"
  "REMOVE rom          - Remove ROM from RIA.\n"
  "REBOOT              - Reboot the RIA. Will load selected boot ROM.\n"
  "RESET               - Start 6502 at current reset vector ($FFFC).\n"
  "MKDIR dir           - Make a new directory.\n"
  "UNLINK file|dir     - Delete a file or empty directory.\n"
  "UPLOAD file         - Write file. Binary chunks follow.\n"
  "BINARY addr len crc - Write memory. Binary data follows.\n"
  "0000 (00 00 ...)    - Read or write memory.")

X(hlp_text_set,
  "Settings:\n"
  "HELP SET attr       - Show information about a setting.\n"
  "SET PHI2 (kHz)      - Query or set PHI2 speed. This is the 6502 clock.\n"
  "SET BOOT (rom|-)    - Select ROM to boot from cold start. \"-\" for none.\n"
  "SET TZ (tz)         - Query or set time zone.\n"
  "SET KB (layout)     - Query or set keyboard layout.\n"
  "SET CP (cp)         - Query or set code page.\n"
  "SET VGA (0|1|2)     - Query or set display type for VGA output."
#ifdef RP6502_RIA_W
  "\n"
  "SET RF (0|1)        - Disable or enable radio.\n"
  "SET RFCC (cc|-)     - Set country code for RF devices. \"-\" for worldwide.\n"
  "SET SSID (ssid|-)   - Set SSID for WiFi. \"-\" for none.\n"
  "SET PASS (pass|-)   - Set password for WiFi. \"-\" for none.\n"
  "SET BLE (0|1|2)     - Disable or enable Bluetooth LE. 2 enables pairing."
#endif
  "")

X(hlp_text_about,
  "Picocomputer 6502 - Copyright (c) 2025 Rumbledethumps.\n"
  "     Pi Pico SDKs - Copyright (c) 2020 Raspberry Pi (Trading) Ltd.\n"
  "      Tiny printf - Copyright (c) 2014-2019 Marco Paland, PALANDesign.\n"
  "          TinyUSB - Copyright (c) 2018 hathach (tinyusb.org)\n"
  "          BTstack - Copyright (c) 2009 BlueKitchen GmbH\n"
  "            FatFs - Copyright (c) 20xx ChaN.\n"
  "         littlefs - Copyright (c) 2022 The littlefs authors.\n"
  "                    Copyright (c) 2017 Arm Limited."
// Note that BTstack HID descriptor parsing is used for non-W builds
#ifdef RP6502_RIA_W
  "\n"
  "   CYW43xx driver - Copyright (c) 2019-2022 George Robotics Pty Ltd.\n"
  "             lwIP - Copyright (c) 2001-2002 Swedish Institute of\n"
  "                                            Computer Science."
#endif
  "")

X(hlp_text_system,
  "The Picocomputer does not use a traditional parallel ROM like a 27C64 or\n"
  "similar. Instead, this monitor is used to prepare the 6502 RAM with software\n"
  "that would normally be on a ROM chip. The 6502 is currently in-reset right\n"
  "now; the RESB line is low. What you are seeing is coming from the RP6502 RIA.\n"
  "You can return to this monitor at any time by pressing CTRL-ALT-DEL or sending\n"
  "a break to the serial port. Since these signals are handled by the RP6502 RIA,\n"
  "they will always stop even a crashed 6502. This monitor can do scripted things\n"
  "that are useful for developing software. It also provides interactive commands\n"
  "like typing a hex address to see the corresponding RAM value:\n"
  "]0200\n"
  "0200 DA DA DA DA DA DA DA DA DA DA DA DA DA DA DA DA\n"
  "The 64KB of extended memory (XRAM) is mapped from $10000 to $1FFFF.\n"
  "You can also set memory. For example, to set the reset vector:\n"
  "]FFFC 00 02\n"
  "This is useful for some light debugging, but the real power is from the other\n"
  "commands you can explore with this help system. Have fun!")

X(hlp_text_dir,
  "LS (also aliased as DIR) and CD are used to navigate USB mass storage\n"
  "devices. You can change to a different USB device with 0: to 7:. Use the\n"
  "STATUS command to get a list of mounted drives.")

X(hlp_text_mkdir,
  "MKDIR is used to create new directories. Use UNLINK to remove empty directories.")

X(hlp_text_load,
  "LOAD and INFO read ROM files from a USB drive. A ROM file contains both\n"
  "ASCII information for the user and binary information for the system.\n"
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
  "6502 will be reset (started) immediately after loading.")

X(hlp_text_install,
  "INSTALL and REMOVE manage the ROMs installed in the RP6502 RIA flash memory.\n"
  "ROM files must contain a reset vector to be installed. A list of installed\n"
  "ROMs is shown on the base HELP screen. Once installed, these ROMs become an\n"
  "integrated part of the system and can be loaded manually by simply using their\n"
  "name like any other command. The ROM name must not conflict with any other\n"
  "system command, must start with a letter, and may only contain up to 16 ASCII\n"
  "letters and numbers. If the file contains an extension, it must be \".rp6502\",\n"
  "which will be stripped upon install.")

X(hlp_text_reboot,
  "REBOOT will restart the RP6502 RIA. It does the same thing as pressing a\n"
  "reset button attached to the Pi Pico or interrupting the power supply.")

X(hlp_text_reset,
  "RESET will restart the 6502 by bringing RESB high. This is mainly used for\n"
  "automated testing by a script on another system connected to the console.\n"
  "For example, a build script can compile a program, upload it directly to\n"
  "6502 RAM, start it with this RESET, then optionally continue to send and\n"
  "receive data to ensure proper operation of the program.")

X(hlp_text_upload,
  "UPLOAD is used to send a file from another system to the local filesystem.\n"
  "The file may be any type with any name and will overwrite an existing file\n"
  "of the same name. For example, you can send a ROM file along with other\n"
  "files containing graphics or level data for a game. Then you can LOAD the\n"
  "game and test it. The upload is initiated with a filename.\n"
  "]UPLOAD filename.bin\n"
  "The system will respond with a \"}\" prompt or an error message starting with\n"
  "a \"?\". Any error will abort the upload and return you to the monitor.\n"
  "There is no retry as this is not intended to be used on lossy connections.\n"
  "Specify each chunk with a length, up to 1024 bytes, and CRC-32 which you can\n"
  "compute from any zip library.\n"
  "}$400 $0C0FFEE0\n"
  "Send the binary data and you will get another \"}\" prompt or \"?\" error.\n"
  "The transfer is completed with the END command or a blank line. Your choice.\n"
  "}END\n"
  "You will return to a \"]\" prompt on success or \"?\" error on failure.")

X(hlp_text_unlink,
  "UNLINK removes a file. Its intended use is for scripting on another system\n"
  "connected to the monitor. For example, you might want to delete save data\n"
  "as part of automated testing.")

X(hlp_text_binary,
  "BINARY is the fastest way to get code or data from your build system to the\n"
  "6502 RAM. Use the command \"BINARY addr len crc\" with a maximum length of 1024\n"
  "bytes and the CRC-32 calculated with a zip library. Then send the binary.\n"
  "You will return to a \"]\" prompt on success or \"?\" error on failure.")

X(hlp_text_status,
  "STATUS will show the status of all hardware in and connected to the RIA.")
