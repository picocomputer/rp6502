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

// Monitor prompts
X(STR_MON_PROMPT_NEWLINE, "\30\33[0m\33[?25h\n]")
X(STR_MON_PROMPT, "\30\33[0m\33[?25h]")
// Monitor commands
X(STR_HELP, "help")
X(STR_H, "h")
X(STR_QUESTION_MARK, "?")
X(STR_STATUS, "status")
X(STR_SET, "set")
X(STR_LS, "ls")
X(STR_DIR, "dir")
X(STR_CD, "cd")
X(STR_CHDIR, "chdir")
X(STR_MKDIR, "mkdir")
X(STR_LOAD, "load")
X(STR_INFO, "info")
X(STR_INSTALL, "install")
X(STR_REMOVE, "remove")
X(STR_REBOOT, "reboot")
X(STR_RESET, "reset")
X(STR_UPLOAD, "upload")
X(STR_UNLINK, "unlink")
X(STR_BINARY, "binary")
// Settings keys
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
// Settings interface
X(STR_SET_PHI2_RESPONSE, "PHI2: %ld kHz\n")
X(STR_SET_BOOT_RESPONSE, "BOOT: %s\n")
X(STR_SET_CODE_PAGE_DEV_RESPONSE, "CP  : %d (dev)\n")
X(STR_SET_CODE_PAGE_RESPONSE, "CP  : %d\n")
X(STR_SET_VGA_0_LABEL, "640x480")
X(STR_SET_VGA_1_LABEL, "640x480 | 1280x720")
X(STR_SET_VGA_2_LABEL, "1280x1024")
X(STR_SET_VGA_RESPONSE, "VGA : %s\n")
X(STR_SET_RF_RESPONSE, "RF  : %s\n")
X(STR_SET_RFCC_RESPONSE, "RFCC: %s\n")
X(STR_SET_SSID_RESPONSE, "SSID: %s\n")
X(STR_SET_PASS_RESPONSE, "PASS: %s\n")
X(STR_SET_SSID_PASS_RESPONSE, "SSID: %s\nPASS: %s\n")
X(STR_SET_BLE_RESPONSE, "BLE : %s%s%s\n")
X(STR_SET_KB_RESPONSE, "KB  : %s\n")
