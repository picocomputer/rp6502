/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What a host tells the machine its user did: a key, a character, the lock
 * keys, the pointer, a touch, the wheel, a captured mouse, the focus, a paste,
 * the controllers. Each is an event in the machine's own units, with the
 * host's side already worked out (which key in USB usages, where on the canvas,
 * how far in mouse counts), and each event decides here where it goes: to the
 * keyboard and the terminal, the tablet, the mouse, the gamepad block. Every
 * host raises the same events. One that runs the machine on another thread,
 * as the desktop and the web do, queues them (host/sokol/app/evq.h); one that
 * runs it on its own, as libretro does, raises them directly.
 *
 * Back the other way, event_status says what a host needs of the machine to
 * take its next event on the spot: whether a click captures the pointer,
 * whether the tablet has the pointer and the wheel, which cursor shows.
 */

#ifndef _CORE_SYS_EVENT_H_
#define _CORE_SYS_EVENT_H_

#include "core/hid/gamepad.h"
#include "core/hid/tablet.h"
#include <stdbool.h>
#include <stdint.h>

/* A key going down or up. usage and modifier are USB HID's. guess says the
 * host gave no character for a Ctrl or Alt chord, so it is guessed. acts is
 * false for a key that only goes into the keyboard's bitmap: on the web a
 * paste key, which the browser turns into a paste. */
typedef struct
{
    uint8_t usage, modifier;
    bool down, repeat, guess, acts;
} event_key_t;

/* A character the host typed, printable and not part of a chord. A key that
 * typed already (event_key_t) keeps the character that follows it from being
 * typed again. */
typedef struct
{
    uint32_t codepoint;
    bool repeat;
} event_char_t;

/* The host's pointer, uncaptured: where it is on the canvas, its buttons
 * (TABLET_FLAG_), and its motion in mouse counts. It is the tablet's while
 * the tablet has the pointer, and off the canvas, or gone from the window, it
 * clears the tablet. A host that captures the pointer for the mouse feeds a
 * mapped mouse from it while it is uncaptured; one that never captures gives
 * the mouse events of its own (event_set_shared). host_cursor says the host
 * draws a cursor of its own over the canvas. */
typedef struct
{
    int16_t x, y;
    int32_t dx, dy;
    uint8_t buttons;
    bool inside, moved, left, host_cursor;
} event_pointer_t;

/* The contacts on the canvas. */
#define EVENT_TOUCHES TABLET_MAX_CONTACTS
typedef struct
{
    uint8_t n;
    struct
    {
        int16_t x, y;
    } at[EVENT_TOUCHES];
} event_touch_t;

/* Wheel notches, for the tablet while it has the wheel, otherwise for the
 * captured mouse. */
typedef struct
{
    int16_t wheel, pan;
    bool tablet;
} event_wheel_t;

/* The captured mouse: its motion in counts, or its buttons. */
typedef struct
{
    int32_t dx, dy;
    uint8_t buttons;
    bool moved;
} event_mouse_t;

/* One host controller, in the units gamepad_host_report takes. Each backend
 * says what it knows of the face buttons' labels. */
typedef struct
{
    uint64_t id; /* stable while plugged, so a player keeps its number */
    uint8_t dpad, button0, button1;
    int8_t lx, ly, rx, ry;
    uint8_t lt, rt;
    uint8_t type; /* GAMEPAD_TYPE_ */
    bool sticks;
} gamepad_host_t;

/* The host's controllers as it last read them. Not opened, the host has none
 * to read and every player is unplugged. A host whose controllers come and go
 * gives each its player as it first comes, by its id; a host with fixed ports
 * (fixed) has the player at each pad's place, an id of 0 being no pad. */
typedef struct
{
    uint8_t n;
    bool opened, fixed;
    gamepad_host_t pad[GAMEPAD_PLAYERS];
} event_gamepads_t;

/* A host that never captures the pointer, as libretro and a script are not,
 * shares it: the tablet has it whenever it is wanted (tablet_wanted), and a
 * mapped mouse has events of its own. One that captures it, as a desktop's
 * window and a web page do, gives it to the tablet only while the program
 * has no mouse mapped or the tablet mapped too, and a click otherwise
 * captures it for the mouse. */
void event_set_shared(bool shared);

void event_key(const event_key_t *e);
void event_char(const event_char_t *e);
void event_locks(uint8_t locks); /* KEYBOARD_LED_, as the host has them */
void event_pointer(const event_pointer_t *e);
void event_touch(const event_touch_t *e);
void event_wheel(const event_wheel_t *e);
void event_mouse(const event_mouse_t *e);
void event_focus(bool focused);
void event_paste(const char *utf8); /* a paste the host handed over */
void event_gamepads(const event_gamepads_t *e);

/* Whether a chord acted on the key still down, which a host's close that
 * the same key asked for (Alt-F4 on Windows) is no close. */
bool event_chord_held(void);

/* What a host needs of the machine to take its next event on the spot. */
#define EVENT_MOUSE_MAPPED 0x01    /* a click captures the pointer */
#define EVENT_TO_TABLET 0x02       /* the tablet has the pointer */
#define EVENT_WHEEL_TO_TABLET 0x04 /* and the wheel */
#define EVENT_EXITED 0x08          /* the program has stopped */
#define EVENT_PADS_MAPPED 0x10     /* read the controllers */
typedef struct
{
    uint8_t flags;
    uint8_t cursor; /* TABLET_CURSOR_, OFF while the program draws its own */
} event_status_t;

/* The status as the machine stands, once a frame or so. It also gives the
 * tablet the pointer or takes it (tablet_host_give), and unplugs the
 * controllers once no program has them mapped. */
event_status_t event_status(void);

/* After a state load, which brings back the players the saved machine had:
 * the host's next controllers tell each player again, or unplug it. */
void event_state_loaded(void);

/* Back to the start, for tests. */
void event_reset(void);

#endif /* _CORE_SYS_EVENT_H_ */
