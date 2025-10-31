/*
 * Dead-key support types and constants
 */
#ifndef _RIA_HID_KBD_DEAD_H_
#define _RIA_HID_KBD_DEAD_H_

#include <stdint.h>
#include <stddef.h>

// Accent identifiers
#define KBD_ACC_NONE 0
#define KBD_ACC_ACUTE 1   // ´
#define KBD_ACC_GRAVE 2   // `
#define KBD_ACC_CIRC 3    // ^
#define KBD_ACC_DIAER 4   // ¨
#define KBD_ACC_TILDE 5   // ~
#define KBD_ACC_RING 6    // ˚
#define KBD_ACC_CARON 7   // ˇ
#define KBD_ACC_CEDILLA 8 // ¸

// A dead-key trigger: which key variant activates which accent
typedef struct
{
    uint8_t keycode;       // HID keycode (0-127)
    uint8_t variant;       // 0:plain, 1:shift, 2:AltGr, 3:shift+AltGr
    uint8_t accent;        // KBD_ACC_*
    uint16_t printable_uc; // Spacing accent to emit on fallback (Unicode)
} kbd_deadtrigger_t;

// A composition map entry: accent + base -> combined unicode
typedef struct
{
    uint8_t accent;    // KBD_ACC_*
    uint16_t base;     // base Unicode code point
    uint16_t combined; // combined Unicode code point
} kbd_deadmap_t;

#endif /* _RIA_HID_KBD_DEAD_H_ */
