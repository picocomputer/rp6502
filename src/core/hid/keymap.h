/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The keyboard's terminal half: keys in, console characters out.
 *
 * keyboard.c reports which keys are down and offers each press to this machine's
 * keymap. This answers with a layout, dead keys and a code page. A machine whose
 * host produced the characters first answers with nothing and takes text
 * instead -- src/core/hid/vtkeys.c, a different file and not this contract.
 */

#ifndef _CORE_HID_KEYMAP_H_
#define _CORE_HID_KEYMAP_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Every new key press, and the modifier byte after every report. A firmware
 * answers with a layout; a machine whose host resolved the characters before
 * the keystroke arrived answers with nothing -- src/core/hid/vtkeys.c. The
 * prefix names the answerer, so the declaration lives in the answerer's
 * header and core/hid/keyboard.h stays one module end to end. */
void keymap_on_key(uint8_t modifier, uint8_t keycode);
void keymap_on_modifiers(uint8_t modifier);

void keymap_init(void);

// Auto-repeat: the held key is re-read from keyboard, so a release ends it.
void keymap_task(void);

// Drain the character queue into buf.
size_t keymap_in_chars(char *buf, size_t length);

// Responder prints all keyboard layout options.
int keymap_layouts_response(char *buf, size_t buf_size, int state, unsigned width);


// Configuration setting KB
#define KEYMAP_LAYOUT_LIST_SIZE 40
bool keymap_check_layout_list(const char *in, char *out);
void keymap_apply_layout_list(const char *list, bool changed);
int keymap_layout_list_response(char *buf, size_t buf_size, int state, unsigned width);
const char *keymap_get_layout(void);
const char *keymap_get_layout_verbose(void);

/* This driver's row in a machine's driver list; see core/sys/driver.h. */
#define KEYMAP_CONFIG_LAYOUT_LIST CONFIG_RAW(L, keymap, layout_list, \
    KEYMAP_LAYOUT_LIST_SIZE, "", keymap_check_layout_list, keymap_apply_layout_list, \
    STR_KB, keymap_layout_list_response, STR_HELP_SET_KB, keymap_layouts_response)
#define KEYMAP_DRIVER DRIVER(keymap_init, keymap_task, nul_task, nul_run, nul_stop, nul_break, \
    KEYMAP_CONFIG_LAYOUT_LIST, nul_config)

#endif /* _CORE_HID_KEYMAP_H_ */
