/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* These do not get localized because they are */
/* part of the API guarantee for the monitor.  */

#if !defined(X)
#define X(name, value)
#endif

// Configuration files on littlefs
X(STR_CFG_FILENAME, "CONFIG.SYS")
X(STR_MDM_PHONEBOOK, "PHONE0.SYS")
X(STR_MDM_PHONE_TMP, "PHONE0.TMP")
X(STR_MDM_SETTINGS, "MODEM0.SYS")

// https://www.ntppool.org/
X(STR_NTP_SERVER, "pool.ntp.org")

// VGA driver
X(STR_VGA_DISPLAY_TYPE_0, "640x480")
X(STR_VGA_DISPLAY_TYPE_1, "640x480 | 1280x720")
X(STR_VGA_DISPLAY_TYPE_2, "1280x1024")
X(STR_VGA1, "VGA1")
X(STR_VGA_SEARCHING, "VGA Searching")
X(STR_VGA_VERSION_UNKNOWN, "VGA Version Unknown")
X(STR_VGA_NOT_FOUND, "VGA Not Found")
X(STR_VGA_CONNECTION_LOST, "VGA Connection Lost")

// Status
X(STR_STATUS_WIFI, "WiFi: %s\n")
X(STR_STATUS_MAC, "MAC : %02X:%02X:%02X:%02X:%02X:%02X\n")
X(STR_STATUS_IPV4, "IPv4: %s\n")
X(STR_STATUS_NTP, "NTP : %s\n")
X(STR_STATUS_TIME, "Time: %s\n")
X(STR_STATUS_BLE_FULL, "BLE : %d %s, %d %s, %d %s%s\n")
X(STR_STATUS_BLE_SIMPLE, "BLE : %s\n")
X(STR_STATUS_USB, "USB : %d %s, %d %s, %d %s, %d %s\n")
X(STR_STATUS_MSC, "%s: %.1f %s %.8s %.16s rev %.4s\n")

// ANSI to reset the terminal at boot
X(STR_SYS_FULL_TERM_RESET, "\30\33c\n");
X(STR_SYS_DEBUG_TERM_RESET, "\30\33[0m\33[?25h\n\n");

// Monitor prompts
X(STR_MON_PROMPT_NEWLINE, "\30\33[0m\33[?25h\n]")
X(STR_MON_PROMPT, "\30\33[0m\33[?25h]")

// Monitor keywords
X(STR_POSIX, "POSIX")
X(STR_END, "END")
X(STR_HELP, "HELP")
X(STR_H, "H")
X(STR_QUESTION_MARK, "?")
X(STR_STATUS, "STATUS")
X(STR_SET, "SET")
X(STR_LS, "LS")
X(STR_DIR, "DIR")
X(STR_CD, "CD")
X(STR_CHDIR, "CHDIR")
X(STR_MKDIR, "MKDIR")
X(STR_LOAD, "LOAD")
X(STR_INFO, "INFO")
X(STR_INSTALL, "INSTALL")
X(STR_REMOVE, "REMOVE")
X(STR_REBOOT, "REBOOT")
X(STR_RESET, "RESET")
X(STR_UPLOAD, "UPLOAD")
X(STR_UNLINK, "UNLINK")
X(STR_BINARY, "BINARY")
X(STR_PHI2, "PHI2")
X(STR_BOOT, "BOOT")
X(STR_TZ, "TZ")
X(STR_KB, "KB")
X(STR_CP, "CP")
X(STR_VGA, "VGA")
X(STR_RF, "RF")
X(STR_RFCC, "RFCC")
X(STR_SSID, "SSID")
X(STR_PASS, "PASS")
X(STR_BLE, "BLE")
X(STR_ABOUT, "ABOUT")
X(STR_CREDITS, "CREDITS")
X(STR_SYSTEM, "SYSTEM")
X(STR_0, "0")
X(STR_0000, "0000")
X(STR_0_COLON, "0:")
X(STR_1_COLON, "1:")
X(STR_2_COLON, "2:")
X(STR_3_COLON, "3:")
X(STR_4_COLON, "4:")
X(STR_5_COLON, "5:")
X(STR_6_COLON, "6:")
X(STR_7_COLON, "7:")
X(STR_8_COLON, "8:")
X(STR_9_COLON, "9:")

// Settings interface
X(STR_SET_PHI2_RESPONSE, "PHI2: %ld (kHz)\n")
X(STR_SET_BOOT_RESPONSE, "BOOT: %s\n")
X(STR_SET_TZ_RESPONSE, "TZ  : %s\n")
X(STR_SET_KB_RESPONSE, "KB  : %s (%s)\n")
X(STR_SET_CODE_PAGE_DEV_RESPONSE, "CP  : %d (dev)\n")
X(STR_SET_CODE_PAGE_RESPONSE, "CP  : %d\n")
X(STR_SET_VGA_RESPONSE, "VGA : %d (%s)\n")
X(STR_SET_RF_RESPONSE, "RF  : %d (%s)\n")
X(STR_SET_RFCC_RESPONSE, "RFCC: %s%s(%s)\n")
X(STR_SET_SSID_RESPONSE, "SSID: %s\n")
X(STR_SET_PASS_RESPONSE, "PASS: %s\n")
X(STR_SET_BLE_RESPONSE, "BLE : %d (%s%s%s)\n")

// Virtual modem
X(STR_MDM_RESPONSE_0, "OK")
X(STR_MDM_RESPONSE_1, "CONNECT")
X(STR_MDM_RESPONSE_2, "RING")
X(STR_MDM_RESPONSE_3, "NO CARRIER")
X(STR_MDM_RESPONSE_4, "ERROR")
X(STR_MDM_RESPONSE_5, "CONNECT 1200")
X(STR_MDM_RESPONSE_6, "NO DIALTONE")
X(STR_MDM_RESPONSE_7, "BUSY")
X(STR_MDM_RESPONSE_8, "NO ANSWER")

// The only bit of help we don't localize.
// Note that BTstack HID descriptor parsing is used for non-W builds.
X(STR_HELP_ABOUT,
  "Picocomputer 6502 - Copyright (c) 2025 Rumbledethumps.\n"
  "     Pi Pico SDKs - Copyright (c) 2020 Raspberry Pi (Trading) Ltd.\n"
  "      Tiny printf - Copyright (c) 2014-2019 Marco Paland, PALANDesign.\n"
  "          TinyUSB - Copyright (c) 2018 hathach (tinyusb.org)\n"
  "          BTstack - Copyright (c) 2009 BlueKitchen GmbH\n"
  "            FatFs - Copyright (c) 20xx ChaN.\n"
  "         littlefs - Copyright (c) 2022 The littlefs authors.\n"
  "                    Copyright (c) 2017 Arm Limited.\n"
  "          emu8950 - Copyright (c) 2001-2020 Mitsutaka Okazaki\n"
  "                    Copyright (c) 2021-2022 Graham Sanderson\n"
#ifdef RP6502_RIA_W
  "   CYW43xx driver - Copyright (c) 2019-2022 George Robotics Pty Ltd.\n"
  "             lwIP - Copyright (c) 2001-2002 Swedish Institute of\n"
  "                                            Computer Science.\n"
#endif
  "")
X(STR_HELP_ABOUT_VIP,
  "             VIPs - ")
