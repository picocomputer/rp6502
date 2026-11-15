/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The keyboard driver of a machine with no system under it. Keys to
 * characters: the layout lookup, dead keys, Alt codes and auto-repeat. And
 * the locks, which are this file's: their keys, their LEDs and how they
 * start. Kept apart from keyboard.c because a machine whose host has already
 * done this work links neither this file nor the layout database. */

#include "core/sys/timer.h"
#include "machine.h"
#include "core/sys/ria.h"
#include "core/str/oem.h"
#include "core/str/unicode.h"
#include "core/api/proc.h"
#include "core/hid/keyboard.h"
#include "core/hid/vtkey.h"
#include "core/hid/layout.h"
#include "core/hid/keymap.h"
#include "core/sys/ring.h"
#include "core/hid/usage.h"

#define KEYMAP_REPEAT_DELAY 500000
#define KEYMAP_REPEAT_RATE 30000

static int keymap_layout_index;
static timer_deadline_t keymap_repeat_timer;
static uint8_t keymap_repeat_modifier;
static uint8_t keymap_repeat_keycode;
static RING(char, KEYMAP_KEY_QUEUE_SIZE) keymap_key_queue;
static bool keymap_alt_mode;
static uint8_t keymap_alt_code;
static char keymap_dead_key0;
static char keymap_dead_key1;
/* Checking a dead key is a linear search over OEM bytes, and converting the
 * layout's code points to OEM as it went would cost hundreds of Unicode
 * lookups out of flash per keystroke, so the converted bytes are cached in
 * RAM. */
#define KEYMAP_DEADKEY_CACHE_SIZE 512
static char keymap_deadkey_cache[KEYMAP_DEADKEY_CACHE_SIZE];
static char const (*keymap_cached_dead2)[3];
static char const (*keymap_cached_dead3)[4];
/* Which code page the cache above holds. A separate flag rather than a
 * reserved page number, so that both live in .bss rather than one of them
 * having to be initialized in .data. */
static uint16_t keymap_cache_code_page;
static bool keymap_cache_valid;

/* Everything reaches the queue through here. A run is queued whole or not at
 * all, and a Ctrl-C latches SIGINT before the space is checked, so a break is
 * seen even when the queue is too full to take the byte. */
static void HOST_IN_RAM_FUNC keymap_queue(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (s[i] == 0x03)
            ria_trigger_sigint();
    if (n <= RING_AVAIL(&keymap_key_queue))
        RING_IN(&keymap_key_queue, s, (uint32_t)n);
}

static void HOST_IN_RAM_FUNC keymap_queue_char(char ch)
{
    keymap_queue(&ch, 1);
}

void keymap_use_layout(int index)
{
    keymap_layout_index = index;
    keymap_cache_valid = false;
}

static void keymap_rebuild_code_page_cache(void);

/* This machine is the keyboard driver, so the locks are its own: they go to
 * the keyboard's LEDs as they change. */
static void HOST_IN_RAM_FUNC keymap_set_locks(uint8_t locks)
{
    keyboard_set_locks(locks);
    hid_set_leds(locks);
}

/* A press of NumLock, CapsLock or ScrollLock flips its lock. False for any
 * other key. */
static bool HOST_IN_RAM_FUNC keymap_lock_key(uint8_t keycode)
{
    uint8_t bit;
    if (keycode == HID_KEY_NUM_LOCK)
        bit = KEYBOARD_LED_NUMLOCK;
    else if (keycode == HID_KEY_CAPS_LOCK)
        bit = KEYBOARD_LED_CAPSLOCK;
    else if (keycode == HID_KEY_SCROLL_LOCK)
        bit = KEYBOARD_LED_SCROLLLOCK;
    else
        return false;
    keymap_set_locks(keyboard_get_locks() ^ bit);
    return true;
}

/* An RP6502 boots with NumLock on, the way an IBM AT and Windows do. */
void HOST_IN_FLASH_FUNC keymap_init(void)
{
    keymap_set_locks(KEYBOARD_LED_NUMLOCK);
}

/* The Raspberry Pi Keyboard has no keypad and uses NumLock to lay one over
 * its letter keys, because it was designed for Linux, which boots NumLock
 * off. */
static const struct
{
    uint16_t vid;
    uint16_t pid;
} keymap_numlock_off_at_boot[] = {
    {0x04D9, 0x0006}, // Raspberry Pi Keyboard
};

void HOST_IN_FLASH_FUNC keyboard_on_mount(uint16_t vendor_id, uint16_t product_id)
{
    if (!hid_boot_enumerating())
        return;
    for (size_t k = 0; k < sizeof(keymap_numlock_off_at_boot) / sizeof(keymap_numlock_off_at_boot[0]); k++)
        if (keymap_numlock_off_at_boot[k].vid == vendor_id &&
            keymap_numlock_off_at_boot[k].pid == product_id)
        {
            keymap_set_locks(keyboard_get_locks() & (uint8_t)~KEYBOARD_LED_NUMLOCK);
            break;
        }
}

/* The cache holds OEM bytes, so it is only good for the page it was built
 * from. The page is checked here, on the way into a keystroke, rather than
 * being invalidated by oem.c, because not every machine routes a code page
 * change through a module that could say so. */
static void HOST_IN_RAM_FUNC keymap_cache_ready(void)
{
    if (!keymap_cache_valid || keymap_cache_code_page != oem_get_code_page_run())
        keymap_rebuild_code_page_cache();
}

/* The modifier byte and the locks, decoded once. mod is kept raw because
 * AltGr is the right alt specifically, not either alt. */
typedef struct
{
    uint8_t mod;
    uint8_t keycode;
    bool shift;
    bool alt;
    bool ctrl;
    bool gui;
    bool capslock;
    bool numlock;
} keymap_press_t;

/* Auto-repeat is armed with the key that is physically down, so that
 * keymap_task can ask whether it is still held. */
static void HOST_IN_RAM_FUNC keymap_arm_repeat(uint8_t modifier, uint8_t keycode, bool initial_press)
{
    keymap_repeat_modifier = modifier;
    keymap_repeat_keycode = keycode;
    keymap_repeat_timer = timer_in_us(initial_press ? KEYMAP_REPEAT_DELAY : KEYMAP_REPEAT_RATE);
}

static void HOST_IN_RAM_FUNC keymap_decode(uint8_t modifier, uint8_t keycode, keymap_press_t *k)
{
    k->mod = modifier;
    k->shift = modifier & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT);
    k->alt = modifier & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT);
    k->ctrl = modifier & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL);
    k->gui = modifier & (KEYBOARD_MODIFIER_LEFTGUI | KEYBOARD_MODIFIER_RIGHTGUI);
    k->capslock = keyboard_get_locks() & KEYBOARD_LED_CAPSLOCK;
    k->numlock = keyboard_get_locks() & KEYBOARD_LED_NUMLOCK;
    k->keycode = keycode;
}

/* Alt held over the keypad's digits types an OEM character code one digit at
 * a time. They are digits while NumLock is on and Shift is up. */
static bool HOST_IN_RAM_FUNC keymap_alt_code_key(const keymap_press_t *k)
{
    bool on_pad = k->keycode >= HID_KEY_KEYPAD_1 && k->keycode <= HID_KEY_KEYPAD_0 &&
                  k->numlock && !k->shift;
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

static bool HOST_IN_RAM_FUNC keymap_shifted(const keymap_press_t *k)
{
    bool use_caps_lock = k->keycode < 128 && layout_use_caps(keymap_layout_index, k->keycode);
    return k->shift ^ (k->capslock && use_caps_lock);
}

static char HOST_IN_RAM_FUNC keymap_layout_lookup(const keymap_press_t *k)
{
    if (k->keycode >= 128 || (k->mod & KEYBOARD_MODIFIER_LEFTALT))
        return 0;
    unsigned col = ((k->mod & KEYBOARD_MODIFIER_RIGHTALT) ? LAYOUT_ALTGR : 0) |
                   (keymap_shifted(k) ? LAYOUT_SHIFT : 0);
    return ff_uni2oem(layout_code_point(keymap_layout_index, k->keycode, col),
                      oem_get_code_page_run());
}

void keymap_abandon(void)
{
    RING_SKIP(&keymap_key_queue, RING_LEN(&keymap_key_queue));
    keymap_alt_mode = false;
    keymap_dead_key0 = keymap_dead_key1 = 0;
}

/* Dead keys compose across two or three presses. Space commits the mark on its
 * own, DEL cancels one, and a pair that composes nothing types both. */
static void HOST_IN_RAM_FUNC keymap_compose(char ch)
{
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
    keymap_queue_char(ch);
}

/* Chords and lock keys, which do something other than type. True when one of
 * them acted. */
static bool HOST_IN_RAM_FUNC keymap_chord(const keymap_press_t *k)
{
    if (k->keycode == HID_KEY_SPACE && k->gui)
    {
        layout_list_cycle();
        return true;
    }
    return proc_key_chord(k->keycode, k->ctrl, k->alt) ||
           keymap_lock_key(k->keycode);
}

/* What the encoder made of a key. One byte goes through the composer, so DEL
 * still cancels a dead key. A longer answer is queued whole, after any dead
 * key it found waiting. */
static void HOST_IN_RAM_FUNC keymap_type(const char *seq, int n)
{
    if (n == 1)
    {
        keymap_compose(seq[0]);
        return;
    }
    if (keymap_dead_key0)
        keymap_queue_char(keymap_dead_key0);
    if (keymap_dead_key1)
        keymap_queue_char(keymap_dead_key1);
    keymap_dead_key0 = keymap_dead_key1 = 0;
    keymap_queue(seq, (size_t)n);
}

/* A chord or a lock key acts once and never repeats. The encoder is asked
 * next, so a key sends what it sends on every machine, in the terminal's
 * modes, and the layout is left the keys that have characters. */
static void HOST_IN_RAM_FUNC keymap_queue_key(uint8_t modifier, uint8_t keycode, bool initial_press)
{
    keymap_cache_ready();
    keymap_arm_repeat(modifier, keycode, initial_press);
    keymap_press_t k;
    keymap_decode(modifier, keycode, &k);
    if (keymap_alt_code_key(&k))
        return;
    if (initial_press && keymap_chord(&k))
    {
        keymap_repeat_keycode = 0;
        return;
    }
    uint8_t flags = (uint8_t)((k.shift ? VTKEY_SHIFT : 0) | (k.alt ? VTKEY_ALT : 0) |
                              (k.ctrl ? VTKEY_CTRL : 0));
    char seq[VTKEY_MAX];
    int n = vtkey_encode(seq, k.keycode, flags);
    if (n == VTKEY_CHAR)
    {
        /* An Alt chord on a key that AltGr does not answer is Meta on the
         * plain or shifted character. Any other character is typed without
         * Alt. */
        char ch = keymap_layout_lookup(&k);
        if (k.alt && !ch && k.keycode < 128)
            ch = ff_uni2oem(layout_code_point(keymap_layout_index, k.keycode,
                                              keymap_shifted(&k) ? LAYOUT_SHIFT : LAYOUT_PLAIN),
                            oem_get_code_page_run());
        else
            flags &= (uint8_t)~VTKEY_ALT;
        n = vtkey_char(seq, k.keycode, ch, flags);
    }
    if (n)
        keymap_type(seq, n);
}

void HOST_IN_RAM_FUNC keymap_task(void)
{
    if (keymap_repeat_keycode && timer_passed(keymap_repeat_timer))
    {
        if (vtkey_repeats() &&
            keyboard_key_down(keymap_repeat_keycode) &&
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

/* layout.py refuses a layout whose dead keys do not fit the cache. */
static void HOST_IN_FLASH_FUNC keymap_rebuild_code_page_cache(void)
{
    size_t cache_index = 0;
    uint16_t code_page = oem_get_code_page_run();
    keymap_cache_code_page = code_page;
    keymap_cache_valid = true;
    unsigned count2 = layout_dead2_count(keymap_layout_index);
    unsigned count3 = layout_dead3_count(keymap_layout_index);
    keymap_cached_dead2 = (void *)&keymap_deadkey_cache[cache_index];
    for (unsigned i = 0; i < count2; i++)
        for (unsigned j = 0; j < 3; j++)
            keymap_deadkey_cache[cache_index++] = ff_uni2oem(
                layout_dead2(keymap_layout_index, i, j), code_page);
    keymap_deadkey_cache[cache_index++] = 0;
    keymap_cached_dead3 = (void *)&keymap_deadkey_cache[cache_index];
    for (unsigned i = 0; i < count3; i++)
        for (unsigned j = 0; j < 4; j++)
            keymap_deadkey_cache[cache_index++] = ff_uni2oem(
                layout_dead3(keymap_layout_index, i, j), code_page);
    keymap_deadkey_cache[cache_index] = 0;
}

size_t HOST_IN_RAM_FUNC keymap_in_chars(char *buf, size_t length)
{
    return RING_OUT(&keymap_key_queue, buf, (uint32_t)length);
}

int HOST_IN_RAM_FUNC keymap_in_peek(size_t offset)
{
    return offset < RING_LEN(&keymap_key_queue)
               ? (unsigned char)*RING_AT(&keymap_key_queue, offset)
               : -1;
}

/* Called once per report, so that an Alt code typed while Alt was held is
 * emitted when Alt is released. */
void HOST_IN_RAM_FUNC keyboard_on_modifiers(uint8_t modifier)
{
    if (keymap_alt_mode &&
        !(modifier & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT)))
    {
        keymap_alt_mode = false;
        if (keymap_alt_code)
            keymap_queue_char(keymap_alt_code);
    }
}

void HOST_IN_RAM_FUNC keyboard_on_press(uint8_t modifier, uint8_t keycode)
{
    keymap_queue_key(modifier, keycode, true);
}

