/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Keys to characters: the layout, dead keys, Alt codes, the VT sequences a
 * terminal expects, and auto-repeat. Separate from keyboard.c because a host
 * whose OS has already done this work links neither this nor the layout
 * database -- emu.cmake omits them both. */

#include "core/sys/timer.h"
#include "machine.h"
#include "core/sys/ria.h"
#include "core/sys/sys.h"
#include "core/str/oem.h"
#include "core/str/unicode.h"
#include "core/hid/keyboard.h"
#include "core/hid/layout.h"
#include "core/hid/keymap.h"
#include "core/sys/config.h"
#include "core/hid/usage.h"
#include <stdio.h>
#include <string.h>
/* The case-insensitive compares a layout name is matched with. Named by
 * POSIX rather than by C, and a host that has no such header supplies
 * one — see src/osal/windows. */
#include <strings.h>

#if defined(DEBUG_HID) || defined(DEBUG_HID_KEYBOARD)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define KEYMAP_REPEAT_DELAY 500000
#define KEYMAP_REPEAT_RATE 30000

#define KEYMAP_KEY_QUEUE_SIZE 16

static int keymap_layout_index;
static const char *keymap_layout_pos;
static timer_deadline_t keymap_repeat_timer;
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
        p = keymap_get_layout_list();
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

/* The modifier byte and the lock lamps, decoded once. The keypad remap moves
 * the keycode and shift together, so they travel as one from here on. mod is
 * kept raw because AltGr is RIGHTALT specifically, not either alt. */
typedef struct
{
    uint8_t mod;
    uint8_t keycode;
    bool shift;
    bool alt;
    bool ctrl;
    bool gui;
    bool capslock;
} keymap_press_t;

static void keymap_decode(uint8_t modifier, uint8_t keycode, bool initial_press,
                          keymap_press_t *k)
{
    k->mod = modifier;
    k->shift = modifier & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT);
    k->alt = modifier & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT);
    k->ctrl = modifier & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL);
    k->gui = modifier & (KEYBOARD_MODIFIER_LEFTGUI | KEYBOARD_MODIFIER_RIGHTGUI);
    k->capslock = keyboard_get_leds() & KEYBOARD_LED_CAPSLOCK;
    bool is_numlock = keyboard_get_leds() & KEYBOARD_LED_NUMLOCK;
    /* Armed with the key that is physically down, before the remap below, so
     * keymap_task can ask whether it is still held. */
    keymap_repeat_modifier = modifier;
    keymap_repeat_keycode = keycode;
    keymap_repeat_timer = timer_in_us(initial_press ? KEYMAP_REPEAT_DELAY : KEYMAP_REPEAT_RATE);
    // When not in numlock, and not shifted, remap num pad
    if (keycode >= HID_KEY_KEYPAD_1 &&
        keycode <= HID_KEY_KEYPAD_DECIMAL &&
        (!is_numlock || k->shift))
    {
        if (is_numlock)
            k->shift = false;
        keycode = keyboard_keypad_nav(keycode);
    }
    k->keycode = keycode;
}

// Alt held over the num pad types a code point one digit at a time.
static bool keymap_alt_code_key(const keymap_press_t *k)
{
    bool on_pad = k->keycode >= HID_KEY_KEYPAD_1 && k->keycode <= HID_KEY_KEYPAD_0;
    if (!keymap_alt_mode && !(on_pad && k->alt))
        return false;
    if (!keymap_alt_mode)
    {
        keymap_alt_mode = true;
        keymap_alt_code = 0;
    }
    if (on_pad)
    {
        keymap_alt_code *= 10;
        if (k->keycode < HID_KEY_KEYPAD_0)
            keymap_alt_code += k->keycode - HID_KEY_KEYPAD_1 + 1;
    }
    return true;
}

static bool keymap_shifted(const keymap_press_t *k)
{
    bool use_caps_lock = k->keycode < 128 && layout_use_caps(keymap_layout_index, k->keycode);
    return k->shift ^ (k->capslock && use_caps_lock);
}

// The plain, shifted or AltGr character this key carries, 0 for none.
static char keymap_layout_lookup(const keymap_press_t *k)
{
    if (k->keycode >= 128 || (k->mod & (KEYBOARD_MODIFIER_LEFTALT |
                                        KEYBOARD_MODIFIER_LEFTGUI |
                                        KEYBOARD_MODIFIER_RIGHTGUI)))
        return 0;
    unsigned col = ((k->mod & KEYBOARD_MODIFIER_RIGHTALT) ? LAYOUT_ALTGR : 0) |
                   (keymap_shifted(k) ? LAYOUT_SHIFT : 0);
    return ff_uni2oem(layout_code_point(keymap_layout_index, k->keycode, col),
                      oem_get_code_page_run());
}

/* An Alt chord on a key AltGr does not answer: the unmodified character,
 * prefixed with ESC the way xterm sends Meta. */
static bool keymap_alt_escape(const keymap_press_t *k)
{
    char ch = ff_uni2oem(layout_code_point(keymap_layout_index, k->keycode,
                                           keymap_shifted(k) ? LAYOUT_SHIFT : LAYOUT_PLAIN),
                         oem_get_code_page_run());
    if (k->ctrl)
    {
        char c = keyboard_ctrl_promote(ch, k->keycode);
        if (c)
            ch = c;
    }
    if (!ch)
        return false;
    keymap_queue_char_char('\33', ch);
    return true;
}

// Drop whatever was half-typed; the machine is going somewhere else.
static void keymap_abandon(void)
{
    keymap_key_queue_tail = keymap_key_queue_head;
    keymap_alt_mode = false;
    keymap_dead_key0 = keymap_dead_key1 = 0;
}

/* Dead keys compose across two or three presses. Space commits the mark on
 * its own, DEL cancels one, and an unmatched pair types both. */
static void keymap_compose(char ch)
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
}

// Chords that do something other than type. True when one fired.
static bool keymap_chord(const keymap_press_t *k)
{
    switch (k->keycode)
    {
    case HID_KEY_SPACE:
        if (k->gui)
        {
            keymap_repeat_keycode = 0; // one-shot; never auto-repeats while held
            keymap_cycle_layout();
            return true;
        }
        break;
    case HID_KEY_F4:
        // alt-f4 exits and returns to launcher
        if (k->alt && sys_break_to_launcher())
        {
            keymap_abandon();
            return true;
        }
        break;
    case HID_KEY_DELETE:
        // ctrl-alt-del exits to monitor, where there is one
        if (k->ctrl && k->alt && sys_break())
        {
            keymap_abandon();
            return true;
        }
        break;
    /* The lamps change and the key still falls through to send nothing:
     * ScrollLock is inside the VT table's range with an empty entry. */
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
    return false;
}

static void keymap_emit_vt(const keymap_press_t *k)
{
    char seq[16];
    if (keyboard_vt_seq(seq, sizeof(seq), k->keycode,
                        keyboard_vt_mod(k->shift, k->alt, k->ctrl, k->gui)))
        keymap_queue_str(seq);
}

static void keymap_queue_key(uint8_t modifier, uint8_t keycode, bool initial_press)
{
    keymap_cache_ready();
    keymap_press_t k;
    keymap_decode(modifier, keycode, initial_press, &k);
    if (keymap_alt_code_key(&k))
        return;
    char ch = keymap_layout_lookup(&k);
    if (k.alt && !ch && k.keycode < 128 && keymap_alt_escape(&k))
        return;
    if (k.ctrl)
        ch = keyboard_ctrl_promote(ch, k.keycode);
    // Latch a SIGINT even if com not draining
    if (ch == 0x03)
        ria_trigger_sigint();
    if (ch)
    {
        keymap_compose(ch);
        return;
    }
    if (initial_press && keymap_chord(&k))
        return;
    keymap_emit_vt(&k);
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
    if (keymap_repeat_keycode && timer_passed(keymap_repeat_timer))
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

/* Validate and canonicalize in one pass -- unknown name, duplicate or
 * overflow all refuse -- writing only the caller's buffer. */
bool keymap_check_layout_list(const char *in, char *out)
{
    return keymap_build_layout_list(in, out, KEYMAP_LAYOUT_LIST_SIZE);
}

/* Keep the active layout if it survived the new list, otherwise the first.
 * The position points into config's storage, which holds these same bytes
 * by the time this runs. */
void keymap_apply_layout_list(const char *list, bool changed)
{
    (void)list;
    (void)changed;
    keymap_layout_pos = keymap_find_token(keymap_get_layout_list(), keymap_layout_name);
    if (!keymap_layout_pos)
        keymap_layout_pos = keymap_get_layout_list();
    keymap_apply_active();
}

/* SET's line for this row: the list when there is one, else the layout. */
int keymap_layout_list_response(char *buf, size_t buf_size, int state, unsigned width)
{
    (void)state;
    (void)width;
    const char *list = keymap_get_layout_list();
    if (strchr(list, ' '))
        snprintf(buf, buf_size, STR_SET_KB_LIST_RESPONSE, list);
    else
        snprintf(buf, buf_size, STR_SET_KB_RESPONSE,
                 keymap_get_layout(), keymap_get_layout_verbose());
    return -1;
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
    /* An empty list is not a list. A machine with no stored layout adopts
     * the build default and keeps it, so the file completes itself once. */
    if (!keymap_get_layout_list()[0])
    {
        char name[LAYOUT_NAME_MAX];
        layout_name(keymap_sanitize_layout(""), name);
        keymap_set_layout_list(name);
    }
    else
        keymap_apply_layout_list(keymap_get_layout_list(), true);
}

/* Once per report, so an Alt code committed while Alt was held is emitted
 * when it is released. */
void keymap_on_modifiers(uint8_t modifier)
{
    if (keymap_alt_mode &&
        !(modifier & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT)))
    {
        keymap_alt_mode = false;
        if (keymap_alt_code)
            keymap_queue_char(keymap_alt_code);
    }
}

void keymap_on_key(uint8_t modifier, uint8_t keycode)
{
    keymap_queue_key(modifier, keycode, true);
}

