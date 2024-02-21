/*
 * Copyright (c) 2023 Rumbledethumps
 * 
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _KBD_FRA_H_
#define _KBD_FRA_H_

/*
 * "French Legacy AZERTY" layout
 * (there are also "French Standard AZERTY" & "French Standard BÉPO" layouts)
 * see http://kbdlayout.info/
 * see src/pico-sdk/lib/tinyusb/src/class/hid/hid.h for keycodes
 */

// KEYCODE to Unicode Conversion
// {without shift, with shift, with altGr}

#define HID_KEYCODE_TO_UNICODE_FR        HID_KEYCODE_TO_UNICODE_FRA_AZERTY
#define HID_KEYCODE_TO_UNICODE_FR_FR     HID_KEYCODE_TO_UNICODE_FRA_AZERTY
#define HID_KEYCODE_TO_UNICODE_FR_AZERTY HID_KEYCODE_TO_UNICODE_FRA_AZERTY
#define HID_KEYCODE_TO_UNICODE_FRA       HID_KEYCODE_TO_UNICODE_FRA_AZERTY
#define HID_KEYCODE_TO_UNICODE_FRA_FR    HID_KEYCODE_TO_UNICODE_FRA_AZERTY

#define HID_KEYCODE_TO_UNICODE_FRA_AZERTY                  \
        /* STATUS CODES                                 */ \
        {0, 0, 0},             /* 0x00 NO KEY PRESSED   */ \
        {0, 0, 0},             /* 0x01 ERROR ROLL OVER  */ \
        {0, 0, 0},             /* 0x02 POST FAIL        */ \
        {0, 0, 0},             /* 0x03 UNDEFINED        */ \
        /* LETTERS                                      */ \
        {'q', 'Q', 0},         /* 0x04 (A) FRA          */ \
        {'b', 'B', 0},         /* 0x05                  */ \
        {'c', 'C', 0},         /* 0x06                  */ \
        {'d', 'D', 0},         /* 0x07                  */ \
        {'e', 'E', 0x20AC},    /* 0x08 FRA '€'          */ \
        {'f', 'F', 0},         /* 0x09                  */ \
        {'g', 'G', 0},         /* 0x0a                  */ \
        {'h', 'H', 0},         /* 0x0b                  */ \
        {'i', 'I', 0},         /* 0x0c                  */ \
        {'j', 'J', 0},         /* 0x0d                  */ \
        {'k', 'K', 0},         /* 0x0e                  */ \
        {'l', 'L', 0},         /* 0x0f                  */ \
        {',', '?', 0},         /* 0x10 (M) FRA          */ \
        {'n', 'N', 0},         /* 0x11                  */ \
        {'o', 'O', 0},         /* 0x12                  */ \
        {'p', 'P', 0},         /* 0x13                  */ \
        {'a', 'A', 0},         /* 0x14 (Q) FRA          */ \
        {'r', 'R', 0},         /* 0x15                  */ \
        {'s', 'S', 0},         /* 0x16                  */ \
        {'t', 'T', 0},         /* 0x17                  */ \
        {'u', 'U', 0},         /* 0x18                  */ \
        {'v', 'V', 0},         /* 0x19                  */ \
        {'z', 'Z', 0},         /* 0x1a (W) FRA          */ \
        {'x', 'X', 0},         /* 0x1b                  */ \
        {'y', 'Y', 0},         /* 0x1c                  */ \
        {'w', 'W', 0},         /* 0x1d (Z) FRA          */ \
        /* DIGITS                                       */ \
        {'&', '1', 0},         /* 0x1e FRA              */ \
        {0x00E9, '2', '~'},    /* 0x1f FRA 'é'          */ \
        {'"', '3', '#'},       /* 0x20 FRA              */ \
        {'\'', '4', '{'},      /* 0x21 FRA              */ \
        {'(', '5', '['},       /* 0x22 FRA              */ \
        {'-', '6', '|'},       /* 0x23 FRA              */ \
        {0x00E8, '7', '`'},    /* 0x24 FRA 'è'          */ \
        {'_', '8', '\\'},      /* 0x25 FRA              */ \
        {0x00E7, '9', '^'},    /* 0x26 FRA 'ç'          */ \
        {0x00E0, '0', '@'},    /* 0x27 FRA 'à'          */ \
        {'\r', '\r', 0},       /* 0x28 ENTER / RETURN   */ \
        {'\x1b', '\x1b', 0},   /* 0x29 ESCAPE           */ \
        {'\b', '\b', 0},       /* 0x2a BACKSPACE        */ \
        {'\t', '\t', 0},       /* 0x2b TAB              */ \
        {' ', ' ', 0},         /* 0x2c SPACE            */ \
        {')', 0x00B0, ']'},    /* 0x2d FRA  '°'         */ \
        {'=', '+', '}'},       /* 0x2e FRA              */ \
        {'^', 0x00A8, 0},      /* 0x2f FRA '¨'          */ \
        {'$', 0x00A3, 0x00A4}, /* 0x30 FRA '£' + '¤'    */ \
        {'<', '>', '|'},       /* 0x31 FRA              */ \
        {'*', 0x00B5, 0},      /* 0x32 FRA 'µ'          */ \
        {'m', 'M', 0},         /* 0x33 FRA              */ \
        {'*', 0x00B5, 0},      /* 0x34 FRA 'µ'          */ \
        {0x00B2, 0x00B2, 0},   /* 0x35 FRA '²'          */ \
        {';', '.', 0},         /* 0x36 FRA              */ \
        {':', '/', 0},         /* 0x37 FRA              */ \
        {'!', 0x00A7, 0},      /* 0x38 FRA '§'          */ \
        /* FUNCTION KEYS, ARROWS & OTHERS               */ \
        {0, 0, 0},             /* 0x39 CAPS LOCK        */ \
        {0, 0, 0},             /* 0x3a F1               */ \
        {0, 0, 0},             /* 0x3b F2               */ \
        {0, 0, 0},             /* 0x3c F3               */ \
        {0, 0, 0},             /* 0x3d F4               */ \
        {0, 0, 0},             /* 0x3e F5               */ \
        {0, 0, 0},             /* 0x3f F6               */ \
        {0, 0, 0},             /* 0x40 F7               */ \
        {0, 0, 0},             /* 0x41 F8               */ \
        {0, 0, 0},             /* 0x42 F9               */ \
        {0, 0, 0},             /* 0x43 F10              */ \
        {0, 0, 0},             /* 0x44 F11              */ \
        {0, 0, 0},             /* 0x45 F12              */ \
        {0, 0, 0},             /* 0x46 PRINT SCREEN     */ \
        {0, 0, 0},             /* 0x47 SCROLL LOCK      */ \
        {0, 0, 0},             /* 0x48 PAUSE            */ \
        {0, 0, 0},             /* 0x49 INSERT           */ \
        {0, 0, 0},             /* 0x4a HOME             */ \
        {0, 0, 0},             /* 0x4b PAGE UP          */ \
        {0, 0, 0},             /* 0x4c DELETE           */ \
        {0, 0, 0},             /* 0x4d END              */ \
        {0, 0, 0},             /* 0x4e PAGE DOWN        */ \
        {0, 0, 0},             /* 0x4f RIGHT ARROW      */ \
        {0, 0, 0},             /* 0x50 LEFT ARROW       */ \
        {0, 0, 0},             /* 0x51 DOWN ARROW       */ \
        {0, 0, 0},             /* 0x52 UP ARROW         */ \
        {0, 0, 0},             /* 0x53 NUM LOCK         */ \
        /* KEYPAD & MISC.                               */ \
        {'/', '/', 0},         /* 0x54                  */ \
        {'*', '*', 0},         /* 0x55                  */ \
        {'-', '-', 0},         /* 0x56                  */ \
        {'+', '+', 0},         /* 0x57                  */ \
        {'\r', '\r', 0},       /* 0x58                  */ \
        {'1', 0, 0},           /* 0x59                  */ \
        {'2', 0, 0},           /* 0x5a                  */ \
        {'3', 0, 0},           /* 0x5b                  */ \
        {'4', 0, 0},           /* 0x5c                  */ \
        {'5', '5', 0},         /* 0x5d                  */ \
        {'6', 0, 0},           /* 0x5e                  */ \
        {'7', 0, 0},           /* 0x5f                  */ \
        {'8', 0, 0},           /* 0x60                  */ \
        {'9', 0, 0},           /* 0x61                  */ \
        {'0', 0, 0},           /* 0x62                  */ \
        {'.', 0, 0},           /* 0x63 DOT .            */ \
        {'<', '>', '|'},       /* 0x64 NON-US \ AND |   */ \
        {0, 0, 0},             /* 0x65 APPLICATION      */ \
        {0, 0, 0},             /* 0x66 POWER            */ \
        {'=', '=', 0},         /* 0x67                  */

#endif /* _KBD_FRA_H_ */
