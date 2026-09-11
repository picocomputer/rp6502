/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_HID_KEYBOARD_H_
#define _CORE_HID_KEYBOARD_H_

#include <stddef.h>
#include <stdint.h>
#include "core/sys/sst.h"
#include <stdbool.h>

#include "core/hid/hid.h"

/* A run of consecutive keyboard usages, one bit each. A boot keyboard's
 * modifier byte and an NKRO bitmap are both declared this way. */
#define KEYBOARD_KEY_RUNS 4
typedef struct
{
    uint16_t bit_pos;  // bit position of usage_min
    uint16_t usage_min;
    uint16_t count;    // 0 ends the list
} keyboard_key_run_t;

#define KEYBOARD_MAX_KEYBOARDS 4

typedef struct keyboard_connection
{
    bool valid;
    int slot;
    uint32_t keys[8];      // last report, one bit per HID usage
    uint8_t report_id;     // when non-zero, the report starts with this byte
    uint16_t codes_offset; // bit offset of the keycode array
    uint8_t codes_count;
    keyboard_key_run_t runs[KEYBOARD_KEY_RUNS];
} keyboard_connection_t;

void keyboard_init(void);
void keyboard_stop(void);

bool keyboard_mount(int slot, const keyboard_connection_t *desc,
                    uint16_t vendor_id, uint16_t product_id);

bool keyboard_umount(int slot);

void keyboard_report(int slot, uint8_t const *data, size_t size);

uint8_t keyboard_get_report_id(int slot);

bool keyboard_xreg(uint16_t word);

bool keyboard_is_mapped(void);

uint8_t keyboard_get_modifier(void);
bool keyboard_key_down(uint8_t keycode);
uint8_t keyboard_get_leds(void);
void keyboard_toggle_lock(uint8_t bit);

/* The lock state as reported by a host that owns it, in KEYBOARD_LED_ bits. */
void keyboard_set_locks(uint8_t leds);

void keyboard_hid_set(uint8_t keycode, bool down);

/* Releases every key and leaves the locks alone. A host that has just
 * restored a savestate calls this because the keys the blob remembers were
 * released in a session this machine is no longer in, so no release event is
 * coming to clear them. A key that is still physically down comes back on its
 * next report. */
void keyboard_release_all(void);

/* What a keypad key navigates to when NumLock is off. Zero for KP5 and for
 * any usage NumLock does not remap. */
uint8_t keyboard_keypad_nav(uint8_t hid_usage);

/* The xterm modifier parameter, the number in ESC[1;{mod}: 1 with nothing
 * held, plus 1 for shift, 2 for alt, 4 for ctrl and 8 for gui. A host whose
 * window manager owns the gui key passes false for it. */
int keyboard_vt_mod(bool shift, bool alt, bool ctrl, bool gui);

/* The escape sequence a key with no character of its own sends, written into
 * the caller's buffer. Zero for any other usage, including Enter, Tab, Escape
 * and Backspace, which have characters, and which character is the machine's
 * to say. The VT220 numbering that makes F5 15 and F6 17 is a table nobody can
 * check by eye, so it lives in one place. */
size_t keyboard_vt_seq(char *out, size_t cap, uint8_t hid_usage, int ansi_mod);

/* What Ctrl makes of a byte: Ctrl-A is 0x01 through Ctrl-Z 0x1A and the
 * punctuation range with it, Backspace is BS where DEL was, and a byte that is
 * C0 already types itself. 0 when Ctrl has nothing to say. Backspace needs the
 * keycode because DEL is outside both promotable ranges; pass HID_KEY_NONE
 * where there is a character but no key. */
char keyboard_ctrl_promote(char ch, uint8_t keycode);

/* The blob carries where the program asked for its bitmap and what is in it.
 * The connection table is not saved, because it describes what is plugged in
 * now rather than what the blob remembers. */
#define KEYBOARD_SST_SIZE 35
void keyboard_sst_save(sst_cursor_t *c, unsigned flags);
bool keyboard_sst_load(sst_cursor_t *c, unsigned flags);

#define KEYBOARD_DRIVER DRIVER(keyboard_init, nul_task, nul_task, nul_run, keyboard_stop, nul_break, \
    nul_config, nul_config, SST(KEYB, 1, KEYBOARD_SST_SIZE, keyboard_sst_save, keyboard_sst_load))

#endif /* _CORE_HID_KEYBOARD_H_ */
