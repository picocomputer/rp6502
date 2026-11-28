/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The event queue between the window and the machine: two queues of
 * messages, one each way, and nothing else the two sides share but the
 * frames (run.h) and, while a debugger is up, the machine's lock.
 *
 * The window is what the host gives the emulator to show and hear it by: the
 * desktop's window on its own thread, the web player's page. The machine is
 * the 6502 and everything around it: on the desktop its own thread, on the
 * web a worker. Neither side calls into the other. The window sends the
 * machine its user's events (core/sys/event.h), and what only a window has:
 * when the display called back, a dropped ROM, its close. The machine tells
 * the window what it needs to take the next event on the spot (whether a
 * click captures the pointer, which cursor, whether a touch is the
 * program's) and what it asked of the host (a copy, a paste, to close).
 *
 * A message is a kind and a body of up to EVQ_MOST bytes, in order. Each
 * queue has one writer and one reader at a time: the debugger's windows,
 * which run on the window's thread holding the machine's lock, may write
 * what the machine would, since the machine is held while they do. On the
 * desktop a queue is a ring in memory, and a message to the machine wakes
 * its thread. On the web the halves share no memory: a message goes to the
 * other half in a batch the page or the worker posts, and lands in that
 * half's queue there (host/web/link.h).
 */

#ifndef _HOST_SOKOL_APP_EVQ_H_
#define _HOST_SOKOL_APP_EVQ_H_

#include "core/sys/event.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The largest body: a pasted text, which sokol caps at 64 KB with its NUL. */
#define EVQ_MOST 65536

typedef enum
{
    /* ---- to the machine: its user's events (core/sys/event.h) ---- */
    EVQ_KEY = 1,  /* event_key_t */
    EVQ_CHAR,     /* event_char_t */
    EVQ_LOCKS,    /* uint8_t, KEYBOARD_LED_ */
    EVQ_TABLET,   /* event_tablet_t */
    EVQ_WHEEL,    /* event_wheel_t, the tablet's */
    EVQ_MOUSE,    /* event_mouse_t */
    EVQ_FOCUS,    /* uint8_t */
    EVQ_FOCUSABLE,/* none: sent once, as the window starts */
    EVQ_PASTE,    /* UTF-8, NUL-terminated: a paste the browser handed over */
    EVQ_GAMEPADS, /* event_gamepads_t */
    /* ---- to the machine: what only a window has ---- */
    EVQ_MINIMIZED,/* uint8_t */
    EVQ_MENU,     /* uint8_t: the drop-a-ROM card is up */
    EVQ_BOOT,     /* a host path, UTF-8, NUL-terminated: a dropped ROM */
    EVQ_TICK,     /* evq_tick_t: a display callback */
    EVQ_CLOSE,    /* none: the window was asked to close */
    /* ---- to the window ---- */
    EVQ_STATUS,   /* evq_status_t */
    EVQ_COPY,     /* UTF-8, NUL-terminated: for the host's clipboard */
    EVQ_BOOTED,   /* none: the dropped ROM booted */
    EVQ_QUIT,     /* uint8_t: close the window, 1 when it was asked to */
} evq_kind_t;

/* A display callback, as the window hands it over (run.c). */
typedef struct
{
    uint64_t at, dt;
    uint32_t n;
    bool paced;
} evq_tick_t;

/* What the window needs of the machine to take an event on the spot
 * (event_status), whether the machine's frames follow the display, and the
 * display's period as the machine measures it, sent whenever any changes. */
typedef struct
{
    event_status_t event;
    bool follows;
    int64_t period_ns;
} evq_status_t;

/* A message as it is taken: its body stays put until the next take on the
 * same side. */
typedef struct
{
    evq_kind_t kind;
    size_t len;
    const void *body;
} evq_msg_t;

/* The window's side. A message to the machine wakes it (evq_set_wake). */
bool evq_to_machine(evq_kind_t kind, const void *body, size_t len);
bool evq_window_take(evq_msg_t *m);

/* The machine's side. */
bool evq_to_window(evq_kind_t kind, const void *body, size_t len);
bool evq_machine_take(evq_msg_t *m);
bool evq_machine_waiting(void);

/* A NUL-terminated text as a message's body. */
bool evq_text_to_machine(evq_kind_t kind, const char *utf8);
bool evq_text_to_window(evq_kind_t kind, const char *utf8);

#ifdef __EMSCRIPTEN__
/* A message the other half of the web player posted, into this half's queue
 * (host/web/link.h). */
void evq_deliver(evq_kind_t kind, const void *body, size_t len);
#endif

/* What wakes the machine's thread, NULL where it has none. */
void evq_set_wake(void (*wake)(void));

/* Both queues emptied, for a run that starts over (tests). */
void evq_reset(void);

#endif /* _HOST_SOKOL_APP_EVQ_H_ */
