/*
 * English (US-International) dead-key tables
 */
#ifndef _RIA_HID_KBD_ENG_INTL_H_
#define _RIA_HID_KBD_ENG_INTL_H_

#include "hid/kbd_dead.h"

// Reuse US QWERTY keymap for base characters
#include "hid/kbd_eng.h"
#define KBD_HID_KEY_TO_UNICODE_EN_US_INTL        KBD_HID_KEY_TO_UNICODE_ENG_US_QWERTY
#define KBD_HID_KEY_TO_UNICODE_ENG_US_INTL       KBD_HID_KEY_TO_UNICODE_ENG_US_QWERTY
#define KBD_HID_KEY_TO_UNICODE_ENG_US_INTERNATIONAL KBD_HID_KEY_TO_UNICODE_ENG_US_QWERTY

// Triggers: which physical keys act as dead keys in US-International
// keycode values come from the HID usage table used in kbd.c
// 0x34: quote (' ")
// 0x35: backtick/tilde (` ~)
// 0x23: 6 / caret (^)
static const kbd_deadtrigger_t __in_flash("ria_hid_kbd") kbd_eng_intl_dead_triggers[] = {
    // ' (acute) and " (diaeresis)
    {0x34, 0, KBD_ACC_ACUTE,    0x00B4}, // ' -> ´ acute accent (spacing)
    {0x34, 1, KBD_ACC_DIAER,    0x00A8}, // " -> ¨ diaeresis (spacing)
    // ` (grave) and ~ (tilde)
    {0x35, 0, KBD_ACC_GRAVE,    0x0060}, // ` -> ` grave (spacing backtick)
    {0x35, 1, KBD_ACC_TILDE,    0x007E}, // ~ -> ~ tilde (spacing)
    // ^ via Shift+6
    {0x23, 1, KBD_ACC_CIRC,     0x005E}, // ^ -> ^ circumflex (spacing)
};

// Composition maps: a subset of common combinations supported by CP437/Latin-1
static const kbd_deadmap_t __in_flash("ria_hid_kbd") kbd_eng_intl_dead_maps[] = {
    // Acute
    {KBD_ACC_ACUTE,  'a', 0x00E1}, {KBD_ACC_ACUTE, 'A', 0x00C1},
    {KBD_ACC_ACUTE,  'e', 0x00E9}, {KBD_ACC_ACUTE, 'E', 0x00C9},
    {KBD_ACC_ACUTE,  'i', 0x00ED}, {KBD_ACC_ACUTE, 'I', 0x00CD},
    {KBD_ACC_ACUTE,  'o', 0x00F3}, {KBD_ACC_ACUTE, 'O', 0x00D3},
    {KBD_ACC_ACUTE,  'u', 0x00FA}, {KBD_ACC_ACUTE, 'U', 0x00DA},
    {KBD_ACC_ACUTE,  'y', 0x00FD}, {KBD_ACC_ACUTE, 'Y', 0x00DD},

    // Grave
    {KBD_ACC_GRAVE,  'a', 0x00E0}, {KBD_ACC_GRAVE, 'A', 0x00C0},
    {KBD_ACC_GRAVE,  'e', 0x00E8}, {KBD_ACC_GRAVE, 'E', 0x00C8},
    {KBD_ACC_GRAVE,  'i', 0x00EC}, {KBD_ACC_GRAVE, 'I', 0x00CC},
    {KBD_ACC_GRAVE,  'o', 0x00F2}, {KBD_ACC_GRAVE, 'O', 0x00D2},
    {KBD_ACC_GRAVE,  'u', 0x00F9}, {KBD_ACC_GRAVE, 'U', 0x00D9},

    // Circumflex
    {KBD_ACC_CIRC,   'a', 0x00E2}, {KBD_ACC_CIRC,  'A', 0x00C2},
    {KBD_ACC_CIRC,   'e', 0x00EA}, {KBD_ACC_CIRC,  'E', 0x00CA},
    {KBD_ACC_CIRC,   'i', 0x00EE}, {KBD_ACC_CIRC,  'I', 0x00CE},
    {KBD_ACC_CIRC,   'o', 0x00F4}, {KBD_ACC_CIRC,  'O', 0x00D4},
    {KBD_ACC_CIRC,   'u', 0x00FB}, {KBD_ACC_CIRC,  'U', 0x00DB},

    // Diaeresis
    {KBD_ACC_DIAER,  'a', 0x00E4}, {KBD_ACC_DIAER, 'A', 0x00C4},
    {KBD_ACC_DIAER,  'e', 0x00EB}, {KBD_ACC_DIAER, 'E', 0x00CB},
    {KBD_ACC_DIAER,  'i', 0x00EF}, {KBD_ACC_DIAER, 'I', 0x00CF},
    {KBD_ACC_DIAER,  'o', 0x00F6}, {KBD_ACC_DIAER, 'O', 0x00D6},
    {KBD_ACC_DIAER,  'u', 0x00FC}, {KBD_ACC_DIAER, 'U', 0x00DC},
    {KBD_ACC_DIAER,  'y', 0x00FF},

    // Tilde
    {KBD_ACC_TILDE,  'n', 0x00F1}, {KBD_ACC_TILDE, 'N', 0x00D1},
    {KBD_ACC_TILDE,  'a', 0x00E3}, {KBD_ACC_TILDE, 'A', 0x00C3},
    {KBD_ACC_TILDE,  'o', 0x00F5}, {KBD_ACC_TILDE, 'O', 0x00D5},
};

// Macro to select these tables when RP6502_KEYBOARD is ENG_US_INTL
#define KBD_DEAD_SELECT_ENG_US_INTL                                \
    static const kbd_deadtrigger_t *kbd_dead_triggers = kbd_eng_intl_dead_triggers; \
    static size_t kbd_dead_triggers_count = sizeof(kbd_eng_intl_dead_triggers)/sizeof(kbd_eng_intl_dead_triggers[0]); \
    static const kbd_deadmap_t *kbd_dead_maps = kbd_eng_intl_dead_maps;            \
    static size_t kbd_dead_maps_count = sizeof(kbd_eng_intl_dead_maps)/sizeof(kbd_eng_intl_dead_maps[0]);

#endif /* _RIA_HID_KBD_ENG_INTL_H_ */
