/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Keys to characters: the layout, dead keys, Alt codes, the VT sequences a
 * terminal expects, and auto-repeat. Separate from keyboard.c because a host
 * whose OS has already done this work links neither this nor the layout
 * database -- emu.cmake omits them both. */

#include "core/main.h"
#include "core/api/oem.h"
#include "core/api/uni.h"
#include "core/hid/keyboard.h"
#include "core/hid/layout.h"
#include "core/hid/keymap.h"
#include "core/hid/vt.h"
#include "core/hid/usage.h"
#include "core/cfg.h"
#include "host.h"
#include <stdio.h>
#include <string.h>
/* The case-insensitive compares a layout name is matched with. Named by
 * POSIX rather than by C, and a host that has no such header supplies
 * one — see src/host/windows. */
#include <strings.h>

#if defined(DEBUG_RIA_HID) || defined(DEBUG_RIA_HID_KEYBOARD)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define KEYMAP_REPEAT_DELAY 500000
#define KEYMAP_REPEAT_RATE 30000

#define KEYMAP_KEY_QUEUE_SIZE 16

static bool keymap_layout_loaded;
static int keymap_layout_index;
static char keymap_layout_list[KEYMAP_LAYOUT_LIST_SIZE];
static const char *keymap_layout_pos;
static host_deadline_t keymap_repeat_timer;
static uint8_t keymap_repeat_modifier;
static uint8_t keymap_repeat_keycode;
static char keymap_key_queue[KEYMAP_KEY_QUEUE_SIZE];
static uint8_t keymap_key_queue_head;
static uint8_t keymap_key_queue_tail;
static bool keymap_alt_mode;
static uint8_t keymap_alt_code;
static char keymap_dead_key0;
static char keymap_dead_key1;
// Dead keys checks need a linear search with oem (8-bit) chars.
// This can require hundreds of unicode lookups from flash.
// To make this faster, we cache the oem chars in RAM.
#define KEYMAP_DEADKEY_CACHE_SIZE 512
static char keymap_deadkey_cache[KEYMAP_DEADKEY_CACHE_SIZE];
static char const (*keymap_cached_dead2)[3];
static char const (*keymap_cached_dead3)[4];
/* Which page the cache above was built for. A flag rather than a sentinel page,
 * so both live in .bss instead of one of them in .data. */
static uint16_t keymap_cache_code_page;
static bool keymap_cache_valid;

// The active layout's name and description, copied out of the database
// so the settings pattern can keep returning a pointer.
static char keymap_layout_name[LAYOUT_NAME_MAX];
static char keymap_layout_description[LAYOUT_DESC_MAX];

static void keymap_queue_str(const char *str)
{
    // All or nothing
    size_t len = strlen(str);
    size_t used = (KEYMAP_KEY_QUEUE_SIZE + keymap_key_queue_head - keymap_key_queue_tail) % KEYMAP_KEY_QUEUE_SIZE;
    if (len > KEYMAP_KEY_QUEUE_SIZE - 1 - used)
        return;
    while (*str)
    {
        keymap_key_queue_head = (keymap_key_queue_head + 1) % KEYMAP_KEY_QUEUE_SIZE;
        keymap_key_queue[keymap_key_queue_head] = *str++;
    }
}

static void keymap_queue_char(char ch)
{
    if ((keymap_key_queue_head + 1) % KEYMAP_KEY_QUEUE_SIZE != keymap_key_queue_tail)
    {
        keymap_key_queue_head = (keymap_key_queue_head + 1) % KEYMAP_KEY_QUEUE_SIZE;
        keymap_key_queue[keymap_key_queue_head] = ch;
    }
}

static void keymap_queue_char_char(char ch0, char ch1)
{
    if ((keymap_key_queue_head + 1) % KEYMAP_KEY_QUEUE_SIZE != keymap_key_queue_tail &&
        (keymap_key_queue_head + 2) % KEYMAP_KEY_QUEUE_SIZE != keymap_key_queue_tail)
    {
        keymap_key_queue_head = (keymap_key_queue_head + 1) % KEYMAP_KEY_QUEUE_SIZE;
        keymap_key_queue[keymap_key_queue_head] = ch0;
        keymap_key_queue_head = (keymap_key_queue_head + 1) % KEYMAP_KEY_QUEUE_SIZE;
        keymap_key_queue[keymap_key_queue_head] = ch1;
    }
}

/* The shared promotion, plus the one a keycode answers: this machine is
 * holding the key, not the character it typed. */
static char keymap_ctrl_promote(char ch, uint8_t keycode)
{
    char c = vt_ctrl_promote(ch);
    if (c)
        return c;
    if (keycode == HID_KEY_BACKSPACE)
        return '\b';
    /* Enter, Tab and Escape are C0 controls already, so Ctrl has nothing left
     * to promote and the key still types itself. The console keymap defines no
     * control form for these three for the same reason. */
    if ((unsigned char)ch < 0x20)
        return ch;
    return 0;
}

// Resolve keymap_layout_index from keymap_layout_pos and rebuild the cache.
static void keymap_apply_active(void)
{
    size_t len = 0;
    while (keymap_layout_pos[len] && keymap_layout_pos[len] != ' ')
        len++;
    for (int i = 0; i < layout_count(); i++)
    {
        char name[LAYOUT_NAME_MAX];
        layout_name(i, name);
        if (strlen(name) == len && !strncmp(keymap_layout_pos, name, len))
        {
            keymap_layout_index = i;
            break;
        }
    }
    layout_name(keymap_layout_index, keymap_layout_name);
    layout_description(keymap_layout_index, keymap_layout_description);
    keymap_cache_valid = false;
}

static void keymap_cycle_layout(void)
{
    const char *p = keymap_layout_pos;
    while (*p && *p != ' ')
        p++;
    while (*p == ' ')
        p++;
    if (!*p)
        p = keymap_layout_list;
    if (p == keymap_layout_pos)
        return;
    keymap_layout_pos = p;
    keymap_apply_active();
}

/* The cache holds OEM bytes, so it is only good for the page it was built from.
 * Checked here rather than driven from oem.c: not every machine routes a code
 * page change through a module that could tell us -- the Pocket's is its font's
 * -- and on the RIA the USB task runs before this one, so a keystroke could
 * otherwise beat the rebuild by a whole pass of the loop. */
static void keymap_rebuild_code_page_cache(void);

static void keymap_cache_ready(void)
{
    if (!keymap_cache_valid || keymap_cache_code_page != oem_get_code_page_run())
        keymap_rebuild_code_page_cache();
}

static void keymap_queue_key(uint8_t modifier, uint8_t keycode, bool initial_press)
{
    keymap_cache_ready();
    bool key_shift = modifier & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT);
    bool key_alt = modifier & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT);
    bool key_ctrl = modifier & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL);
    bool key_gui = modifier & (KEYBOARD_MODIFIER_LEFTGUI | KEYBOARD_MODIFIER_RIGHTGUI);
    bool is_numlock = keyboard_get_leds() & KEYBOARD_LED_NUMLOCK;
    bool is_capslock = keyboard_get_leds() & KEYBOARD_LED_CAPSLOCK;
    // Set up for repeat
    keymap_repeat_modifier = modifier;
    keymap_repeat_keycode = keycode;
    keymap_repeat_timer = host_deadline_us(initial_press ? KEYMAP_REPEAT_DELAY : KEYMAP_REPEAT_RATE);
    // When not in numlock, and not shifted, remap num pad
    if (keycode >= HID_KEY_KEYPAD_1 &&
        keycode <= HID_KEY_KEYPAD_DECIMAL &&
        (!is_numlock || key_shift))
    {
        if (is_numlock)
            key_shift = false;
        keycode = keyboard_keypad_nav(keycode);
    }
    // ALT codes
    if (keymap_alt_mode || (keycode >= HID_KEY_KEYPAD_1 &&
                         keycode <= HID_KEY_KEYPAD_0 &&
                         key_alt))
    {
        if (!keymap_alt_mode)
        {
            keymap_alt_mode = true;
            keymap_alt_code = 0;
        }
        if (keycode >= HID_KEY_KEYPAD_1 && keycode <= HID_KEY_KEYPAD_0)
        {
            keymap_alt_code *= 10;
            if (keycode < HID_KEY_KEYPAD_0)
                keymap_alt_code += keycode - HID_KEY_KEYPAD_1 + 1;
        }
        return;
    }
    // Shift and caps lock logic
    bool use_caps_lock = keycode < 128 && layout_use_caps(keymap_layout_index, keycode);
    bool is_shifted = key_shift ^ (is_capslock && use_caps_lock);
    // Find plain typed or AltGr character
    uint16_t code_page = oem_get_code_page_run();
    char ch = 0;
    if (keycode < 128 && !(modifier & (KEYBOARD_MODIFIER_LEFTALT |
                                       KEYBOARD_MODIFIER_LEFTGUI |
                                       KEYBOARD_MODIFIER_RIGHTGUI)))
    {
        unsigned col = ((modifier & KEYBOARD_MODIFIER_RIGHTALT) ? LAYOUT_ALTGR : 0) |
                       (is_shifted ? LAYOUT_SHIFT : 0);
        ch = ff_uni2oem(layout_code_point(keymap_layout_index, keycode, col), code_page);
    }
    // ALT characters not found in AltGr get escaped
    if (key_alt && !ch && keycode < 128)
    {
        ch = ff_uni2oem(layout_code_point(keymap_layout_index, keycode,
                                          is_shifted ? LAYOUT_SHIFT : LAYOUT_PLAIN),
                        code_page);
        if (key_ctrl)
        {
            char c = keymap_ctrl_promote(ch, keycode);
            if (c)
                ch = c;
        }
        if (ch)
        {
            keymap_queue_char_char('\33', ch);
            return;
        }
    }
    // Promote ctrl characters
    if (key_ctrl)
        ch = keymap_ctrl_promote(ch, keycode);
    // Latch a SIGINT even if com not draining
    if (ch == 0x03)
        ria_trigger_sigint();
    // Process a regularly typed key
    if (ch)
    {
        // Check for dead key start
        if (!keymap_dead_key0)
        {
            for (int i = 0; keymap_cached_dead2[i][0]; i++)
            {
                if (ch == keymap_cached_dead2[i][0])
                {
                    keymap_dead_key0 = ch;
                    return;
                }
            }
            for (int i = 0; keymap_cached_dead3[i][0]; i++)
            {
                if (ch == keymap_cached_dead3[i][0] ||
                    ch == keymap_cached_dead3[i][1])
                {
                    keymap_dead_key0 = ch;
                    return;
                }
            }
        }
        // Handle second press in dead key sequence
        if (keymap_dead_key0 && !keymap_dead_key1)
        {
            if (ch == ' ')
            {
                keymap_queue_char(keymap_dead_key0);
                keymap_dead_key0 = 0;
                return;
            }
            if (ch == 0x7F)
            {
                keymap_dead_key0 = 0;
                return;
            }
            for (int i = 0; keymap_cached_dead2[i][0]; i++)
            {
                if (keymap_dead_key0 == keymap_cached_dead2[i][0] &&
                    ch == keymap_cached_dead2[i][1])
                {
                    char result = keymap_cached_dead2[i][2];
                    if (!result)
                        break;
                    keymap_queue_char(result);
                    keymap_dead_key0 = 0;
                    return;
                }
            }
            for (int i = 0; keymap_cached_dead3[i][0]; i++)
            {
                if ((keymap_dead_key0 == keymap_cached_dead3[i][0] && ch == keymap_cached_dead3[i][1]) ||
                    (keymap_dead_key0 == keymap_cached_dead3[i][1] && ch == keymap_cached_dead3[i][0]))
                {
                    keymap_dead_key1 = ch;
                    return;
                }
            }
            keymap_queue_char(keymap_dead_key0);
            keymap_queue_char(ch);
            keymap_dead_key0 = 0;
            return;
        }
        // Handle third press in dead key sequence
        if (keymap_dead_key0 && keymap_dead_key1)
        {
            if (ch == ' ')
            {
                keymap_queue_char(keymap_dead_key0);
                keymap_queue_char(keymap_dead_key1);
                keymap_dead_key0 = keymap_dead_key1 = 0;
                return;
            }
            if (ch == 0x7F)
            {
                keymap_dead_key1 = 0;
                return;
            }
            for (int i = 0; keymap_cached_dead3[i][0]; i++)
            {
                if (((keymap_dead_key0 == keymap_cached_dead3[i][0] && keymap_dead_key1 == keymap_cached_dead3[i][1]) ||
                     (keymap_dead_key0 == keymap_cached_dead3[i][1] && keymap_dead_key1 == keymap_cached_dead3[i][0])) &&
                    ch == keymap_cached_dead3[i][2])
                {
                    char result = keymap_cached_dead3[i][3];
                    if (!result)
                        break;
                    keymap_queue_char(result);
                    keymap_dead_key0 = keymap_dead_key1 = 0;
                    return;
                }
            }
            keymap_queue_char(keymap_dead_key0);
            keymap_queue_char(keymap_dead_key1);
            keymap_queue_char(ch);
            keymap_dead_key0 = keymap_dead_key1 = 0;
            return;
        }
        // Not in dead key sequence
        keymap_queue_char(ch);
        return;
    }
    // Non-repeating special key handler
    if (initial_press)
    {
        switch (keycode)
        {
        case HID_KEY_SPACE:
            if (key_gui)
            {
                keymap_repeat_keycode = 0; // one-shot; never auto-repeats while held
                keymap_cycle_layout();
                return;
            }
            break;
        case HID_KEY_F4:
            // alt-f4 exits and returns to launcher
            if (key_alt && main_break_to_launcher())
            {
                keymap_key_queue_tail = keymap_key_queue_head;
                keymap_alt_mode = false;
                keymap_dead_key0 = keymap_dead_key1 = 0;
                return;
            }
            break;
        case HID_KEY_DELETE:
            // ctrl-alt-del exits to monitor, where there is one
            if (key_ctrl && key_alt && main_break())
            {
                keymap_key_queue_tail = keymap_key_queue_head;
                keymap_alt_mode = false;
                keymap_dead_key0 = keymap_dead_key1 = 0;
                return;
            }
            break;
        case HID_KEY_NUM_LOCK:
            keyboard_toggle_lock(KEYBOARD_LED_NUMLOCK);
            break;
        case HID_KEY_CAPS_LOCK:
            keyboard_toggle_lock(KEYBOARD_LED_CAPSLOCK);
            break;
        case HID_KEY_SCROLL_LOCK:
            keyboard_toggle_lock(KEYBOARD_LED_SCROLLLOCK);
            break;
        }
    }
    // Modifier key annotation
    int ansi_modifier = vt_ansi_mod(key_shift, key_alt, key_ctrl, key_gui);
    char seq[16];
    if (vt_key(seq, sizeof(seq), keycode, ansi_modifier))
        keymap_queue_str(seq);
}

static int keymap_sanitize_layout(const char *kb)
{
    int default_index = 0;
    int found_index = -1;
    for (int i = 0; i < layout_count(); i++)
    {
        char name[LAYOUT_NAME_MAX];
        layout_name(i, name);
        if (!strcasecmp(name, "US"))
            default_index = i;
        if (!strcasecmp(name, kb))
            found_index = i;
    }
    if (found_index < 0)
        return default_index;
    else
        return found_index;
}

// Find name as a whole token within a space separated list.
static const char *keymap_find_token(const char *list, const char *name)
{
    size_t name_len = strlen(name);
    while (*list)
    {
        while (*list == ' ')
            list++;
        if (!*list)
            break;
        size_t len = 0;
        while (list[len] && list[len] != ' ')
            len++;
        if (len == name_len && !strncmp(list, name, len))
            return list;
        list += len;
    }
    return NULL;
}

// Validate and canonicalize a space separated layout list into out.
// Fails on an unknown or duplicate layout, an empty list, or overflow.
static bool keymap_build_layout_list(const char *in, char *out, size_t size)
{
    size_t len = 0;
    out[0] = 0;
    while (*in)
    {
        while (*in == ' ')
            in++;
        if (!*in)
            break;
        size_t tok_len = 0;
        while (in[tok_len] && in[tok_len] != ' ')
            tok_len++;
        char name[LAYOUT_NAME_MAX];
        name[0] = 0;
        for (int i = 0; i < layout_count(); i++)
        {
            layout_name(i, name);
            if (strlen(name) == tok_len && !strncasecmp(in, name, tok_len))
                break;
            name[0] = 0;
        }
        if (!name[0] || keymap_find_token(out, name))
            return false;
        if (len + (len ? 1 : 0) + strlen(name) + 1 > size)
            return false;
        if (len)
            out[len++] = ' ';
        strcpy(out + len, name);
        len += strlen(name);
        in += tok_len;
    }
    return len != 0;
}

void keymap_task(void)
{
    if (keymap_repeat_keycode && host_deadline_passed(keymap_repeat_timer))
    {
        if (keyboard_key_down(keymap_repeat_keycode) &&
            keyboard_get_modifier() == keymap_repeat_modifier)
        {
            keymap_queue_key(keyboard_get_modifier(), keymap_repeat_keycode, false);
        }
        else
        {
            keymap_repeat_keycode = 0;
        }
    }
}

/* The width the caller would like is ignored: the list sets its own
 * from the longest name it has. Named rather than left off, because
 * this file is compiled by MSVC now that tests/hid links it. */
int keymap_layouts_response(char *buf, size_t buf_size, int state, unsigned width)
{
    (void)width;
    if (state < 0 || state >= layout_count())
        return -1;
    char name[LAYOUT_NAME_MAX];
    char desc[LAYOUT_DESC_MAX];
    int maxlen = 0;
    for (int i = 0; i < layout_count(); i++)
    {
        layout_name(i, name);
        int thislen = strlen(name);
        if (thislen > maxlen)
            maxlen = thislen;
    }
    layout_name(state, name);
    layout_description(state, desc);
    snprintf(buf, buf_size, "  %*s - \a%s\n", maxlen, name, desc);
    return state + 1;
}

static void keymap_rebuild_code_page_cache(void)
{
    size_t cache_index = 0;
    uint16_t code_page = oem_get_code_page_run();
    keymap_cache_code_page = code_page;
    keymap_cache_valid = true;
    unsigned count2 = layout_dead2_count(keymap_layout_index);
    unsigned count3 = layout_dead3_count(keymap_layout_index);
    keymap_cached_dead2 = (void *)&keymap_deadkey_cache[cache_index];
    for (unsigned i = 0; i < count2; i++)
    {
        for (unsigned j = 0; j < 3; j++)
        {
            keymap_deadkey_cache[cache_index] = ff_uni2oem(
                layout_dead2(keymap_layout_index, i, j), code_page);
            if (++cache_index >= sizeof(keymap_deadkey_cache))
                goto overflow_error;
        }
    }
    keymap_deadkey_cache[cache_index] = 0;
    if (++cache_index >= sizeof(keymap_deadkey_cache))
        goto overflow_error;
    keymap_cached_dead3 = (void *)&keymap_deadkey_cache[cache_index];
    for (unsigned i = 0; i < count3; i++)
    {
        for (unsigned j = 0; j < 4; j++)
        {
            keymap_deadkey_cache[cache_index] = ff_uni2oem(
                layout_dead3(keymap_layout_index, i, j), code_page);
            if (++cache_index >= sizeof(keymap_deadkey_cache))
                goto overflow_error;
        }
    }
    keymap_deadkey_cache[cache_index] = 0;
    return;
overflow_error:
    // Unreachable for a database keyboard_layout_gen.py built: it refuses a
    // layout whose dead keys do not fit here. A machine staging one it
    // did not build loses the composing, not the keyboard.
    keymap_cached_dead2 = (void *)&keymap_deadkey_cache[0];
    keymap_cached_dead3 = (void *)&keymap_deadkey_cache[0];
    keymap_deadkey_cache[0] = 0;
    DBG("keyboard: dead key cache overflow\n");
}

size_t keymap_in_chars(char *buf, size_t length)
{
    size_t i = 0;
    while (i < length && keymap_key_queue_tail != keymap_key_queue_head)
    {
        keymap_key_queue_tail = (keymap_key_queue_tail + 1) % KEYMAP_KEY_QUEUE_SIZE;
        buf[i++] = keymap_key_queue[keymap_key_queue_tail];
    }
    return i;
}

void keymap_load_layout(const char *str)
{
    if (!keymap_build_layout_list(str, keymap_layout_list, sizeof keymap_layout_list))
        layout_name(keymap_sanitize_layout(""), keymap_layout_list);
    keymap_layout_pos = keymap_layout_list;
    keymap_layout_loaded = true;
    keymap_apply_active();
}

bool keymap_set_layout(const char *list)
{
    char buf[KEYMAP_LAYOUT_LIST_SIZE];
    if (!keymap_build_layout_list(list, buf, sizeof buf))
        return false;
    if (!strcmp(buf, keymap_layout_list))
        return true;
    strcpy(keymap_layout_list, buf);
    // Keep the active layout if it survived, otherwise the first.
    keymap_layout_pos = keymap_find_token(keymap_layout_list, keymap_layout_name);
    if (!keymap_layout_pos)
        keymap_layout_pos = keymap_layout_list;
    keymap_apply_active();
    cfg_save();
    return true;
}

const char *keymap_get_layout_list(void)
{
    return keymap_layout_list;
}

const char *keymap_get_layout(void)
{
    return keymap_layout_name;
}

const char *keymap_get_layout_verbose(void)
{
    return keymap_layout_description;
}

void HOST_IN_FLASH("keymap_init") keymap_init(void)
{
    if (!keymap_layout_loaded)
    {
        layout_name(keymap_sanitize_layout(""), keymap_layout_list);
        keymap_layout_pos = keymap_layout_list;
        keymap_apply_active();
    }
}

/* Once per report, so an Alt code committed while Alt was held is emitted
 * when it is released. */
void keyboard_spell_modifiers(uint8_t modifier)
{
    if (keymap_alt_mode &&
        !(modifier & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT)))
    {
        keymap_alt_mode = false;
        if (keymap_alt_code)
            keymap_queue_char(keymap_alt_code);
    }
}

void keyboard_spell_key(uint8_t modifier, uint8_t keycode)
{
    keymap_queue_key(modifier, keycode, true);
}

