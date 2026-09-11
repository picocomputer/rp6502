/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The card a desktop shows when it has no ROM. Its frame and text are drawn
 * rather than shipped as an image, so they are sharp at any window size; the
 * icon on it is the 64x64 from icon.c. Only a platform that can receive a
 * dropped file calls prompt_setup.
 */

#ifndef _HOST_SOKOL_APP_PROMPT_H_
#define _HOST_SOKOL_APP_PROMPT_H_

#include <stdbool.h>

/* Stand up the text and vector renderers. Call once from host_window_init,
 * which the sokol init callback runs after sg_setup. */
void prompt_setup(void);

/* Draw the centered card into the current swapchain pass. It goes with
 * host_window_menu_active being true, which is what suppresses the canvas. */
void prompt_draw(const char *line1, const char *line2);

/* True when a framebuffer-pixel point is over the documentation link. The
 * link's rectangle is set by prompt_draw, so it means something only while the
 * prompt is the overlay on screen. */
bool prompt_url_hit(float x, float y);

/* Open that link in the user's browser. */
void prompt_url_open(void);

#endif /* _HOST_SOKOL_APP_PROMPT_H_ */
