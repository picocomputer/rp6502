/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The web player's two halves and what carries the event queue between them
 * (host/sokol/app/evq.h). One rp6502.wasm runs twice for each player: on the
 * page as the window, with sokol_app, the drawing and the input, and in a
 * worker as the machine, with the 6502, its files and its sound. Nothing that
 * holds up the page holds up the machine: a window being dragged, a slow
 * present, a busy page. The halves share no memory. Each message the queue
 * sends is put in a batch that the page's script posts to the worker, or the
 * worker's to the page, at the end of what it is doing, and the other side
 * puts the batch in its own queue. A finished frame goes to the page the same
 * way, as a copy of its pixels and its shape.
 */

#ifndef _HOST_WEB_LINK_H_
#define _HOST_WEB_LINK_H_

#include "host/sokol/app/run.h"
#include "host/sokol/app/evq.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Whether this instance is the worker's machine rather than the page's
 * window. */
bool link_machine(void);

/* A message to the other half. */
bool link_post(evq_kind_t kind, const void *body, size_t len);

/* The machine's finished frame, to the page. */
void link_post_frame(const run_frame_t *f);

/* The worker's machine, booted, runs from here on what wakes it: a message
 * from the page, the sound's player, or the time its next frame comes due.
 * Returns at once; the machine lives on in the worker. */
int link_run(uint32_t *fb, bool exit_on_halt);

#endif /* _HOST_WEB_LINK_H_ */
