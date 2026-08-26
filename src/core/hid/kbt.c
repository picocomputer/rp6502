/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Keys to characters: the layout, dead keys, Alt codes, the VT sequences a
 * terminal expects, and auto-repeat. Separate from keyboard.c because a host whose
 * OS has already done this work links kbt_null.c instead and drops the layout
 * database with it. */

#include "core/main.h"
#include "core/api/oem.h"
#include "core/api/uni.h"
#include "core/hid/keyboard.h"
#include "core/hid/layout.h"
#include "core/hid/kbt.h"
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

#define KBT_REPEAT_DELAY 500000
#define KBT_REPEAT_RATE 30000

#define KBT_KEY_QUEUE_SIZE 16

static bool kbt_layout_loaded;
static int kbt_layout_index;
static char kbt_layout_list[KBT_LAYOUT_LIST_SIZE];
static const char *kbt_layout_pos;
static host_deadline_t kbt_repeat_timer;
static uint8_t kbt_repeat_modifier;
static uint8_t kbt_repeat_keycode;
static char kbt_key_queue[KBT_KEY_QUEUE_SIZE];
static uint8_t kbt_key_queue_head;
static uint8_t kbt_key_queue_tail;
static bool kbt_alt_mode;
static uint8_t kbt_alt_code;
static char kbt_dead_key0;
static char kbt_dead_key1;
// Dead keys checks need a linear search with oem (8-bit) chars.
// This can require hundreds of unicode lookups from flash.
// To make this faster, we cache the oem chars in RAM.
#define KBT_DEADKEY_CACHE_SIZE 512
static char kbt_deadkey_cache[KBT_DEADKEY_CACHE_SIZE];
static char const (*kbt_cached_dead2)[3];
static char const (*kbt_cached_dead3)[4];
/* Which page the cache above was built for. A flag rather than a sentinel page,
 * so both live in .bss instead of one of them in .data. */
static uint16_t kbt_cache_code_page;
static bool kbt_cache_valid;

// The active layout's name and description, copied out of the database
// so the settings pattern can keep returning a pointer.
static char kbt_layout_name[LAYOUT_NAME_MAX];
static char kbt_layout_description[LAYOUT_DESC_MAX];

static void kbt_queue_str(const char *str)
{
    // All or nothing
    size_t len = strlen(str);
    size_t used = (KBT_KEY_QUEUE_SIZE + kbt_key_queue_head - kbt_key_queue_tail) % KBT_KEY_QUEUE_SIZE;
    if (len > KBT_KEY_QUEUE_SIZE - 1 - used)
        return;
    while (*str)
    {
        kbt_key_queue_head = (kbt_key_queue_head + 1) % KBT_KEY_QUEUE_SIZE;
        kbt_key_queue[kbt_key_queue_head] = *str++;
    }
}

static void kbt_queue_char(char ch)
{
    if ((kbt_key_queue_head + 1) % KBT_KEY_QUEUE_SIZE != kbt_key_queue_tail)
    {
        kbt_key_queue_head = (kbt_key_queue_head + 1) % KBT_KEY_QUEUE_SIZE;
        kbt_key_queue[kbt_key_queue_head] = ch;
    }
}

static void kbt_queue_char_char(char ch0, char ch1)
{
    if ((kbt_key_queue_head + 1) % KBT_KEY_QUEUE_SIZE != kbt_key_queue_tail &&
        (kbt_key_queue_head + 2) % KBT_KEY_QUEUE_SIZE != kbt_key_queue_tail)
    {
        kbt_key_queue_head = (kbt_key_queue_head + 1) % KBT_KEY_QUEUE_SIZE;
        kbt_key_queue[kbt_key_queue_head] = ch0;
        kbt_key_queue_head = (kbt_key_queue_head + 1) % KBT_KEY_QUEUE_SIZE;
        kbt_key_queue[kbt_key_queue_head] = ch1;
    }
}

/* The shared promotion, plus the one a keycode answers: this machine is
 * holding the key, not the character it typed. */
static char kbt_ctrl_promote(char ch, uint8_t keycode)
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

// Resolve kbt_layout_index from kbt_layout_pos and rebuild the cache.
static void kbt_apply_active(void)
{
    size_t len = 0;
    while (kbt_layout_pos[len] && kbt_layout_pos[len] != ' ')
        len++;
    for (int i = 0; i < layout_count(); i++)
    {
        char name[LAYOUT_NAME_MAX];
        layout_name(i, name);
        if (strlen(name) == len && !strncmp(kbt_layout_pos, name, len))
        {
            kbt_layout_index = i;
            break;
        }
    }
    layout_name(kbt_layout_index, kbt_layout_name);
    layout_description(kbt_layout_index, kbt_layout_description);
    kbt_cache_valid = false;
}

static void kbt_cycle_layout(void)
{
    const char *p = kbt_layout_pos;
    while (*p && *p != ' ')
        p++;
    while (*p == ' ')
        p++;
    if (!*p)
        p = kbt_layout_list;
    if (p == kbt_layout_pos)
        return;
    kbt_layout_pos = p;
    kbt_apply_active();
}

/* The cache holds OEM bytes, so it is only good for the page it was built from.
 * Checked here rather than driven from oem.c: not every machine routes a code
 * page change through a module that could tell us -- the Pocket's is its font's
 * -- and on the RIA the USB task runs before this one, so a keystroke could
 * otherwise beat the rebuild by a whole pass of the loop. */
static void kbt_rebuild_code_page_cache(void);

static void kbt_cache_ready(void)
{
    if (!kbt_cache_valid || kbt_cache_code_page != oem_get_code_page_run())
        kbt_rebuild_code_page_cache();
}

static void kbt_queue_key(uint8_t modifier, uint8_t keycode, bool initial_press)
{
    kbt_cache_ready();
    bool key_shift = modifier & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT);
    bool key_alt = modifier & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT);
    bool key_ctrl = modifier & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL);
    bool key_gui = modifier & (KEYBOARD_MODIFIER_LEFTGUI | KEYBOARD_MODIFIER_RIGHTGUI);
    bool is_numlock = keyboard_get_leds() & KEYBOARD_LED_NUMLOCK;
    bool is_capslock = keyboard_get_leds() & KEYBOARD_LED_CAPSLOCK;
    // Set up for repeat
    kbt_repeat_modifier = modifier;
    kbt_repeat_keycode = keycode;
    kbt_repeat_timer = host_deadline_us(initial_press ? KBT_REPEAT_DELAY : KBT_REPEAT_RATE);
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
    if (kbt_alt_mode || (keycode >= HID_KEY_KEYPAD_1 &&
                         keycode <= HID_KEY_KEYPAD_0 &&
                         key_alt))
    {
        if (!kbt_alt_mode)
        {
            kbt_alt_mode = true;
            kbt_alt_code = 0;
        }
        if (keycode >= HID_KEY_KEYPAD_1 && keycode <= HID_KEY_KEYPAD_0)
        {
            kbt_alt_code *= 10;
            if (keycode < HID_KEY_KEYPAD_0)
                kbt_alt_code += keycode - HID_KEY_KEYPAD_1 + 1;
        }
        return;
    }
    // Shift and caps lock logic
    bool use_caps_lock = keycode < 128 && layout_use_caps(kbt_layout_index, keycode);
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
        ch = ff_uni2oem(layout_code_point(kbt_layout_index, keycode, col), code_page);
    }
    // ALT characters not found in AltGr get escaped
    if (key_alt && !ch && keycode < 128)
    {
        ch = ff_uni2oem(layout_code_point(kbt_layout_index, keycode,
                                       is_shifted ? LAYOUT_SHIFT : LAYOUT_PLAIN),
                        code_page);
        if (key_ctrl)
        {
            char c = kbt_ctrl_promote(ch, keycode);
            if (c)
                ch = c;
        }
        if (ch)
        {
            kbt_queue_char_char('\33', ch);
            return;
        }
    }
    // Promote ctrl characters
    if (key_ctrl)
        ch = kbt_ctrl_promote(ch, keycode);
    // Latch a SIGINT even if com not draining
    if (ch == 0x03)
        ria_trigger_sigint();
    // Process a regularly typed key
    if (ch)
    {
        // Check for dead key start
        if (!kbt_dead_key0)
        {
            for (int i = 0; kbt_cached_dead2[i][0]; i++)
            {
                if (ch == kbt_cached_dead2[i][0])
                {
                    kbt_dead_key0 = ch;
                    return;
                }
            }
            for (int i = 0; kbt_cached_dead3[i][0]; i++)
            {
                if (ch == kbt_cached_dead3[i][0] ||
                    ch == kbt_cached_dead3[i][1])
                {
                    kbt_dead_key0 = ch;
                    return;
                }
            }
        }
        // Handle second press in dead key sequence
        if (kbt_dead_key0 && !kbt_dead_key1)
        {
            if (ch == ' ')
            {
                kbt_queue_char(kbt_dead_key0);
                kbt_dead_key0 = 0;
                return;
            }
            if (ch == 0x7F)
            {
                kbt_dead_key0 = 0;
                return;
            }
            for (int i = 0; kbt_cached_dead2[i][0]; i++)
            {
                if (kbt_dead_key0 == kbt_cached_dead2[i][0] &&
                    ch == kbt_cached_dead2[i][1])
                {
                    char result = kbt_cached_dead2[i][2];
                    if (!result)
                        break;
                    kbt_queue_char(result);
                    kbt_dead_key0 = 0;
                    return;
                }
            }
            for (int i = 0; kbt_cached_dead3[i][0]; i++)
            {
                if ((kbt_dead_key0 == kbt_cached_dead3[i][0] && ch == kbt_cached_dead3[i][1]) ||
                    (kbt_dead_key0 == kbt_cached_dead3[i][1] && ch == kbt_cached_dead3[i][0]))
                {
                    kbt_dead_key1 = ch;
                    return;
                }
            }
            kbt_queue_char(kbt_dead_key0);
            kbt_queue_char(ch);
            kbt_dead_key0 = 0;
            return;
        }
        // Handle third press in dead key sequence
        if (kbt_dead_key0 && kbt_dead_key1)
        {
            if (ch == ' ')
            {
                kbt_queue_char(kbt_dead_key0);
                kbt_queue_char(kbt_dead_key1);
                kbt_dead_key0 = kbt_dead_key1 = 0;
                return;
            }
            if (ch == 0x7F)
            {
                kbt_dead_key1 = 0;
                return;
            }
            for (int i = 0; kbt_cached_dead3[i][0]; i++)
            {
                if (((kbt_dead_key0 == kbt_cached_dead3[i][0] && kbt_dead_key1 == kbt_cached_dead3[i][1]) ||
                     (kbt_dead_key0 == kbt_cached_dead3[i][1] && kbt_dead_key1 == kbt_cached_dead3[i][0])) &&
                    ch == kbt_cached_dead3[i][2])
                {
                    char result = kbt_cached_dead3[i][3];
                    if (!result)
                        break;
                    kbt_queue_char(result);
                    kbt_dead_key0 = kbt_dead_key1 = 0;
                    return;
                }
            }
            kbt_queue_char(kbt_dead_key0);
            kbt_queue_char(kbt_dead_key1);
            kbt_queue_char(ch);
            kbt_dead_key0 = kbt_dead_key1 = 0;
            return;
        }
        // Not in dead key sequence
        kbt_queue_char(ch);
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
                kbt_repeat_keycode = 0; // one-shot; never auto-repeats while held
                kbt_cycle_layout();
                return;
            }
            break;
        case HID_KEY_F4:
            // alt-f4 exits and returns to launcher
            if (key_alt && main_break_to_launcher())
            {
                kbt_key_queue_tail = kbt_key_queue_head;
                kbt_alt_mode = false;
                kbt_dead_key0 = kbt_dead_key1 = 0;
                return;
            }
            break;
        case HID_KEY_DELETE:
            // ctrl-alt-del exits to monitor, where there is one
            if (key_ctrl && key_alt && main_break())
            {
                kbt_key_queue_tail = kbt_key_queue_head;
                kbt_alt_mode = false;
                kbt_dead_key0 = kbt_dead_key1 = 0;
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
        kbt_queue_str(seq);
}

static int kbt_sanitize_layout(const char *kb)
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
static const char *kbt_find_token(const char *list, const char *name)
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
static bool kbt_build_layout_list(const char *in, char *out, size_t size)
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
        if (!name[0] || kbt_find_token(out, name))
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

void kbt_task(void)
{
    if (kbt_repeat_keycode && host_deadline_passed(kbt_repeat_timer))
    {
        if (keyboard_key_down(kbt_repeat_keycode) &&
            keyboard_get_modifier() == kbt_repeat_modifier)
        {
            kbt_queue_key(keyboard_get_modifier(), kbt_repeat_keycode, false);
        }
        else
        {
            kbt_repeat_keycode = 0;
        }
    }
}

/* The width the caller would like is ignored: the list sets its own
 * from the longest name it has. Named rather than left off, because
 * this file is compiled by MSVC now that tests/hid links it. */
int kbt_layouts_response(char *buf, size_t buf_size, int state, unsigned width)
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

static void kbt_rebuild_code_page_cache(void)
{
    size_t cache_index = 0;
    uint16_t code_page = oem_get_code_page_run();
    kbt_cache_code_page = code_page;
    kbt_cache_valid = true;
    unsigned count2 = layout_dead2_count(kbt_layout_index);
    unsigned count3 = layout_dead3_count(kbt_layout_index);
    kbt_cached_dead2 = (void *)&kbt_deadkey_cache[cache_index];
    for (unsigned i = 0; i < count2; i++)
    {
        for (unsigned j = 0; j < 3; j++)
        {
            kbt_deadkey_cache[cache_index] = ff_uni2oem(
                layout_dead2(kbt_layout_index, i, j), code_page);
            if (++cache_index >= sizeof(kbt_deadkey_cache))
                goto overflow_error;
        }
    }
    kbt_deadkey_cache[cache_index] = 0;
    if (++cache_index >= sizeof(kbt_deadkey_cache))
        goto overflow_error;
    kbt_cached_dead3 = (void *)&kbt_deadkey_cache[cache_index];
    for (unsigned i = 0; i < count3; i++)
    {
        for (unsigned j = 0; j < 4; j++)
        {
            kbt_deadkey_cache[cache_index] = ff_uni2oem(
                layout_dead3(kbt_layout_index, i, j), code_page);
            if (++cache_index >= sizeof(kbt_deadkey_cache))
                goto overflow_error;
        }
    }
    kbt_deadkey_cache[cache_index] = 0;
    return;
overflow_error:
    // Unreachable for a database keyboard_layout_gen.py built: it refuses a
    // layout whose dead keys do not fit here. A machine staging one it
    // did not build loses the composing, not the keyboard.
    kbt_cached_dead2 = (void *)&kbt_deadkey_cache[0];
    kbt_cached_dead3 = (void *)&kbt_deadkey_cache[0];
    kbt_deadkey_cache[0] = 0;
    DBG("keyboard: dead key cache overflow\n");
}

size_t kbt_in_chars(char *buf, size_t length)
{
    size_t i = 0;
    while (i < length && kbt_key_queue_tail != kbt_key_queue_head)
    {
        kbt_key_queue_tail = (kbt_key_queue_tail + 1) % KBT_KEY_QUEUE_SIZE;
        buf[i++] = kbt_key_queue[kbt_key_queue_tail];
    }
    return i;
}

void kbt_load_layout(const char *str)
{
    if (!kbt_build_layout_list(str, kbt_layout_list, sizeof kbt_layout_list))
        layout_name(kbt_sanitize_layout(""), kbt_layout_list);
    kbt_layout_pos = kbt_layout_list;
    kbt_layout_loaded = true;
    kbt_apply_active();
}

bool kbt_set_layout(const char *list)
{
    char buf[KBT_LAYOUT_LIST_SIZE];
    if (!kbt_build_layout_list(list, buf, sizeof buf))
        return false;
    if (!strcmp(buf, kbt_layout_list))
        return true;
    strcpy(kbt_layout_list, buf);
    // Keep the active layout if it survived, otherwise the first.
    kbt_layout_pos = kbt_find_token(kbt_layout_list, kbt_layout_name);
    if (!kbt_layout_pos)
        kbt_layout_pos = kbt_layout_list;
    kbt_apply_active();
    cfg_save();
    return true;
}

const char *kbt_get_layout_list(void)
{
    return kbt_layout_list;
}

const char *kbt_get_layout(void)
{
    return kbt_layout_name;
}

const char *kbt_get_layout_verbose(void)
{
    return kbt_layout_description;
}

void HOST_IN_FLASH("kbt_init") kbt_init(void)
{
    if (!kbt_layout_loaded)
    {
        layout_name(kbt_sanitize_layout(""), kbt_layout_list);
        kbt_layout_pos = kbt_layout_list;
        kbt_apply_active();
    }
}

/* Once per report, so an Alt code committed while Alt was held is emitted
 * when it is released. */
void keyboard_spell_modifiers(uint8_t modifier)
{
    if (kbt_alt_mode &&
        !(modifier & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT)))
    {
        kbt_alt_mode = false;
        if (kbt_alt_code)
            kbt_queue_char(kbt_alt_code);
    }
}

void keyboard_spell_key(uint8_t modifier, uint8_t keycode)
{
    kbt_queue_key(modifier, keycode, true);
}

