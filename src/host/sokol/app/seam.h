/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The seam between the window and the machine: two queues of messages, one
 * each way, and nothing else the two sides share but the frames (run.h) and,
 * while a debugger is up, the machine's lock.
 *
 * The window is what the host gives the emulator to show and hear it by: the
 * desktop's window on its own thread, the web player's page. The machine is
 * the 6502 and everything around it: on the desktop its own thread, on the web
 * a worker. Neither side calls into the
 * other. The window tells the machine what the user did (keys, pointer,
 * gamepads, a pasted text, a dropped ROM) and when the display called back;
 * the machine tells the window what it needs to know to take the next event
 * on the spot (whether a click captures the pointer, which cursor, whether a
 * touch is the program's) and what it asked of the host (a copy, a paste, to
 * close).
 *
 * A message is a kind and a body of up to SEAM_MOST bytes, in order. Each
 * queue has one writer and one reader at a time: the debugger's windows,
 * which run on the window's thread holding the machine's lock, may write
 * what the machine would, since the machine is held while they do. On the desktop a queue is a ring in
 * memory, and a message to the machine wakes its thread. On the web the
 * halves share no memory: a message goes to the other half in a batch the
 * page or the worker posts, and lands in that half's queue there
 * (host/web/link.h).
 */

#ifndef _HOST_SOKOL_APP_SEAM_H_
#define _HOST_SOKOL_APP_SEAM_H_

#include "host/sokol/app/entry.h"
#include "core/hid/gamepad.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The largest body: a pasted text, which sokol caps at 64 KB with its NUL. */
#define SEAM_MOST 65536

typedef enum
{
    /* ---- to the machine ---- */
    SEAM_KEY = 1,  /* seam_key_t */
    SEAM_CHAR,     /* seam_char_t */
    SEAM_LOCKS,    /* uint8_t, KEYBOARD_LED_ */
    SEAM_POINTER,  /* seam_pointer_t: the pointer while the tablet has it */
    SEAM_TOUCH,    /* seam_touch_t */
    SEAM_WHEEL,    /* seam_wheel_t */
    SEAM_MOUSE,    /* seam_mouse_t: the captured mouse */
    SEAM_FOCUS,    /* uint8_t */
    SEAM_MINIMIZED,/* uint8_t */
    SEAM_MENU,     /* uint8_t: the drop-a-ROM card is up */
    SEAM_PASTE,    /* UTF-8, NUL-terminated: a paste the browser handed over */
    SEAM_CLIPBOARD,/* UTF-8, NUL-terminated: the host's clipboard, as asked */
    SEAM_GAMEPADS, /* seam_gamepads_t */
    SEAM_BOOT,     /* a host path, UTF-8, NUL-terminated: a dropped ROM */
    SEAM_TICK,     /* seam_tick_t: a display callback */
    SEAM_CLOSE,    /* none: the window was asked to close */
    /* ---- to the window ---- */
    SEAM_STATUS,   /* seam_status_t */
    SEAM_COPY,     /* UTF-8, NUL-terminated: for the host's clipboard */
    SEAM_ASK,      /* none: the machine wants the host's clipboard */
    SEAM_BOOTED,   /* none: the dropped ROM booted */
    SEAM_QUIT,     /* uint8_t: close the window, 1 when it was asked to */
} seam_kind_t;

/* A key going down or up. The window has made the host's key a USB usage and
 * its modifiers HID's, and says whether a Ctrl or Alt chord's character is to
 * be guessed and whether the key acts at all beyond the keyboard's bitmap: on
 * the web a paste key is the browser's. */
typedef struct
{
    uint8_t usage, modifier;
    bool down, repeat, guess, acts;
} seam_key_t;

/* A character the host typed, already one the machine takes. */
typedef struct
{
    uint32_t codepoint;
    bool repeat;
} seam_char_t;

/* The host pointer while the tablet has it: where it is on the canvas, its
 * buttons, and the motion a mapped mouse is fed, in its counts. A pointer
 * off the canvas, or gone from the window, clears the tablet. */
typedef struct
{
    int16_t x, y;
    int32_t dx, dy;
    uint8_t buttons; /* TABLET_FLAG_ */
    bool inside, moved, left;
} seam_pointer_t;

/* The contacts on the canvas, as many as sokol reports. */
#define SEAM_TOUCHES 8
typedef struct
{
    uint8_t n;
    struct
    {
        int16_t x, y;
    } at[SEAM_TOUCHES];
} seam_touch_t;

/* Wheel notches, for the tablet while it has the wheel, otherwise for the
 * captured mouse. */
typedef struct
{
    int16_t wheel, pan;
    bool tablet;
} seam_wheel_t;

/* The captured mouse: its motion in counts, or its buttons. */
typedef struct
{
    int32_t dx, dy;
    uint8_t buttons;
    bool moved;
} seam_mouse_t;

/* The host's controllers as the window last polled them. Not opened, the
 * window has none to read and every player is unplugged. */
typedef struct
{
    uint8_t n;
    bool opened;
    gamepad_host_t pad[GAMEPAD_PLAYERS];
} seam_gamepads_t;

/* A display callback, as the window hands it over (run.c). */
typedef struct
{
    uint64_t at, dt;
    uint32_t n;
    bool paced;
} seam_tick_t;

/* What the window needs of the machine to take an event on the spot, sent
 * whenever it changes. */
#define SEAM_MOUSE_MAPPED 0x01    /* a click captures the pointer */
#define SEAM_TO_TABLET 0x02       /* the tablet has the pointer */
#define SEAM_WHEEL_TO_TABLET 0x04 /* and the wheel */
#define SEAM_EXITED 0x08          /* the program has stopped */
#define SEAM_PADS_MAPPED 0x10     /* poll the controllers */
#define SEAM_FOLLOWS 0x20         /* the machine's frames follow the display */
typedef struct
{
    uint8_t flags;
    uint8_t cursor; /* TABLET_CURSOR_, OFF while the program draws its own */
    int64_t period_ns;
} seam_status_t;

/* A message as it is taken: its body stays put until the next take on the
 * same side. */
typedef struct
{
    seam_kind_t kind;
    size_t len;
    const void *body;
} seam_msg_t;

/* The window's side. A message to the machine wakes it (seam_set_wake). */
bool seam_to_machine(seam_kind_t kind, const void *body, size_t len);
bool seam_window_take(seam_msg_t *m);

/* The machine's side. */
bool seam_to_window(seam_kind_t kind, const void *body, size_t len);
bool seam_machine_take(seam_msg_t *m);
bool seam_machine_waiting(void);

/* A NUL-terminated text as a message's body. */
bool seam_text_to_machine(seam_kind_t kind, const char *utf8);
bool seam_text_to_window(seam_kind_t kind, const char *utf8);

#ifdef __EMSCRIPTEN__
/* A message the other half of the web player posted, into this half's queue
 * (host/web/link.h). */
void seam_deliver(seam_kind_t kind, const void *body, size_t len);
#endif

/* What wakes the machine's thread, NULL where it has none. */
void seam_set_wake(void (*wake)(void));

/* Both queues emptied, for a run that starts over (tests). */
void seam_reset(void);

#endif /* _HOST_SOKOL_APP_SEAM_H_ */
