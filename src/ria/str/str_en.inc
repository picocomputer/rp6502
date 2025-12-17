/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* English */

#if !defined(X)
#define X(name, value)
#endif

// Time format
X(STR_STRFTIME, "%c %z %Z")

// Only used in unreachable code
X(STR_ERR_INTERNAL_ERROR, "?Internal error\n")
X(STR_INTERNAL_ERROR, "internal error")

// Monitor errors
X(STR_ERR_MONITOR_RESPONSE_OVERFLOW, "?Monitor response overflow\n")
X(STR_ERR_DEAD_KEY_CACHE_OVERFLOW, "?Dead key cache overflow\n")
X(STR_ERR_UNKNOWN_COMMAND, "?Unknown command\n")
X(STR_ERR_INVALID_ARGUMENT, "?Invalid argument\n")
X(STR_ERR_NO_HELP_FOUND, "?No help found\n")
X(STR_ERR_CYW_FAILED_TO_INIT, "?CYW43 failed to init\n")
X(STR_ERR_VGA_CONNECTION_LOST, "?VGA connection lost\n")

// WiFi status
X(STR_WFI_NOT_CONFIGURED, "not configured")
X(STR_WFI_WAITING, "waiting")
X(STR_WFI_JOINING, "joining")
X(STR_WFI_GETTING_IP, "getting IP")
X(STR_WFI_CONNECTING, "connecting")
X(STR_WFI_CONNECTED, "connected")
X(STR_WFI_NO_IP_ADDRESS, "no IP address")
X(STR_WFI_SSID_NOT_FOUND, "ssid not found")
X(STR_WFI_AUTH_FAILED, "auth failed")
X(STR_WFI_CONNECT_FAILED, "connect failed")

// Network Time Protocol status
X(STR_NTP_NO_NETWORK, "no network")
X(STR_NTP_DNS_LOOKUP, "DNS lookup")
X(STR_NTP_DNS_FAIL, "DNS fail")
X(STR_NTP_REQUESTED, "requested")
X(STR_NTP_REQUEST_TIMEOUT, "request timeout")
X(STR_NTP_SET_TIME_FAILURE, "set time failure")
X(STR_NTP_SUCCESS, "success")

// Status fragments
X(STR_ON, "On")
X(STR_OFF, "Off")
X(STR_RF_OFF, "RF off")
X(STR_ENABLED, "Enabled")
X(STR_DISABLED, "Disabled")
X(STR_WORLDWIDE, "Worldwide")
X(STR_PARENS_NONE, "(none)")
X(STR_PARENS_SET, "(set)")
X(STR_BLE_PAIRING, ", pairing")
X(STR_BLE_NO_RF, ", no RF")

// Device names
X(STR_KEYBOARD_SINGULAR, "keyboard")
X(STR_KEYBOARD_PLURAL, "keyboards")
X(STR_MOUSE_SINGULAR, "mouse")
X(STR_MOUSE_PLURAL, "mice")
X(STR_GAMEPAD_SINGULAR, "gamepad")
X(STR_GAMEPAD_PLURAL, "gamepads")
X(STR_STORAGE_SINGULAR, "storage")
X(STR_STORAGE_PLURAL, "storage")

X(STR_HELP_HELP,
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
  "0000 (00 00 ...)    - Read or write memory.\n")

X(STR_HELP_SET,
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
  "SET BLE (0|1|2)     - Disable or enable Bluetooth LE. 2 enables pairing.\n"
#endif
  "")

X(STR_HELP_ABOUT,
  "Picocomputer 6502 - Copyright (c) 2025 Rumbledethumps.\n"
  "     Pi Pico SDKs - Copyright (c) 2020 Raspberry Pi (Trading) Ltd.\n"
  "      Tiny printf - Copyright (c) 2014-2019 Marco Paland, PALANDesign.\n"
  "          TinyUSB - Copyright (c) 2018 hathach (tinyusb.org)\n"
  "          BTstack - Copyright (c) 2009 BlueKitchen GmbH\n"
  "            FatFs - Copyright (c) 20xx ChaN.\n"
  "         littlefs - Copyright (c) 2022 The littlefs authors.\n"
  "                    Copyright (c) 2017 Arm Limited.\n"
// Note that BTstack HID descriptor parsing is used for non-W builds
#ifdef RP6502_RIA_W
  "\n"
  "   CYW43xx driver - Copyright (c) 2019-2022 George Robotics Pty Ltd.\n"
  "             lwIP - Copyright (c) 2001-2002 Swedish Institute of\n"
  "                                            Computer Science.\n"
#endif
  "")

X(STR_HELP_SYSTEM,
  "The Picocomputer does not use a traditional parallel ROM like a 27C64 or\n"
  "similar. Instead, this monitor is used to prepare the 6502 RAM with software\n"
  "that would normally be on a ROM chip. The 6502 is currently in-reset right\n"
  "now; the RESB line is low. What you are seeing is coming from the RP6502-RIA.\n"
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
  "commands you can explore with this help system. Have fun!\n")

X(STR_HELP_DIR,
  "LS (also aliased as DIR) and CD are used to navigate USB mass storage\n"
  "devices. You can change to a different USB device with 0: to 7:. Use the\n"
  "STATUS command to get a list of mounted drives.\n")

X(STR_HELP_MKDIR,
  "MKDIR is used to create new directories. Use UNLINK to remove empty directories.\n")

X(STR_HELP_LOAD,
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
  "6502 will be reset (started) immediately after loading.\n")

X(STR_HELP_INSTALL,
  "INSTALL and REMOVE manage the ROMs installed in the RP6502 RIA flash memory.\n"
  "ROM files must contain a reset vector to be installed. A list of installed\n"
  "ROMs is shown on the base HELP screen. Once installed, these ROMs become an\n"
  "integrated part of the system and can be loaded manually by simply using their\n"
  "name like any other command. The ROM name must not conflict with any other\n"
  "system command, must start with a letter, and may only contain up to 16 ASCII\n"
  "letters and numbers. If the file contains an extension, it must be \".rp6502\",\n"
  "which will be stripped upon install.\n")

X(STR_HELP_REBOOT,
  "REBOOT will restart the RP6502-RIA. It does the same thing as pressing a\n"
  "reboot button attached to the RIA RUN pin or interrupting the power supply.\n")

X(STR_HELP_RESET,
  "RESET will start the 6502 by bringing RESB high. This monitor will be\n"
  "suspended until the 6502 stops with a UART break, CTRL-ALT-DEL, or exit().\n")

X(STR_HELP_UPLOAD,
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
  "You will return to a \"]\" prompt on success or \"?\" error on failure.\n")

X(STR_HELP_UNLINK,
  "UNLINK removes a file. Its intended use is for scripting on another system\n"
  "connected to the monitor. For example, you might want to delete save data\n"
  "as part of automated testing.")

X(STR_HELP_BINARY,
  "BINARY is the fastest way to get code or data from your build system to the\n"
  "6502 RAM. Use the command \"BINARY addr len crc\" with a maximum length of 1024\n"
  "bytes and the CRC-32 calculated with a zip library. Then send the binary.\n"
  "You will return to a \"]\" prompt on success or \"?\" error on failure.\n")

X(STR_HELP_STATUS,
  "STATUS will show the status of all hardware in and connected to the RIA.\n")

X(STR_HELP_SET_PHI2,
  "PHI2 is the 6502 clock speed in kHz. The valid range is " STR_PHI2_MIN_MAX " but not all\n"
  "frequencies are available. In that case, the next highest frequency will\n"
  "be automatically calculated and selected. Setting is saved on the RIA flash.\n")

X(STR_HELP_SET_BOOT,
  "BOOT selects an installed ROM to be automatically loaded and started when the\n"
  "system is powered up or rebooted. For example, you might want the system to\n"
  "immediately boot into BASIC or an operating system CLI. Using \"-\" for the\n"
  "argument will have the system boot into the monitor you are using now.\n"
  "Setting is saved on the RIA flash.\n")

X(STR_HELP_SET_TZ,
  "TZ sets the time zone using the same format as POSIX. The default is \"UTC0\".\n"
  "Some examples are \"PST8PDT,M3.2.0/2,M11.1.0/2\" for USA Pacific time and \n"
  "\"CET-1CEST,M3.5.0/2,M10.5.0/3\" for Central European time.\n"
  "The easiest way to get this is to ask an AI \"posix tz for {your location}\".\n")

X(STR_HELP_SET_KB,
  "SET KB selects a keyboard layout. e.g. SET KB US\n")

X(STR_HELP_SET_CP,
  "SET CP selects a code page for system text. The following is supported:\n"
  "437, 720, 737, 771, 775, 850, 852, 855, 857, 860, 861, 862, 863, 864, 865,\n"
  "866, 869, 932, 936, 949, 950.  Code pages 720, 932, 936, 949, 950 do not have\n"
  "VGA fonts.\n"
#if RP6502_CODE_PAGE
  "This is a development build. Only " STR_RP6502_CODE_PAGE " is available.\n"
#endif
  "")

X(STR_HELP_SET_VGA,
  "SET VGA selects the display type for VGA output. All canvas resolutions are\n"
  "supported by all display types. Display type is used to maintain square\n"
  "pixels and minimize letterboxing. Note that 1280x1024 is 5:4 so 4:3 graphics\n"
  "will be letterboxed slightly but you'll get 2 extra rows on the terminal.\n"
  "  0 - 640x480, for 4:3 displays\n"
  "  1 - 640x480 and 1280x720, for 16:9 displays\n"
  "  2 - 1280x1024, for 5:4 SXGA displays\n")

X(STR_HELP_SET_RF,
  "SET RF (0|1) turns all radios off or on.")

X(STR_HELP_SET_RFCC,
  "Set this so the CYW43 can use the best radio frequencies for your country.\n"
  "Using \"-\" will clear the country code and default to a worldwide setting.\n"
  "Valid country codes: AU, AT, BE, BR, CA, CL, CN, CO, CZ, DK, EE, FI, FR, DE,\n"
  "GR, HK, HU, IS, IN, IL, IT, JP, KE, LV, LI, LT, LU, MY, MT, MX, NL, NZ, NG,\n"
  "NO, PE, PH, PL, PT, SG, SK, SI, ZA, KR, ES, SE, CH, TW, TH, TR, GB, US.\n")

X(STR_HELP_SET_SSID,
  "This is the Service Set Identifier for your WiFi network. Setting \"-\" will\n"
  "disable WiFi.\n")

X(STR_HELP_SET_PASS,
  "This is the password for your WiFi network. Use \"-\" to clear password.\n")

X(STR_HELP_SET_BLE,
  "Setting 0 disables Bluetooth LE. Setting 1 enables. Setting 2 enters pairing\n"
  "mode which will remain active until successful.\n")
