/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The card a desktop shows when it has no ROM: drawn, not an image, so it is
 * sharp at any window size and carries no asset. Only the platforms that can
 * receive a dropped file set it up, so web and Android link none of it.
 */

#ifndef _HOST_SOKOL_APP_PROMPT_H_
#define _HOST_SOKOL_APP_PROMPT_H_

#include <stdbool.h>

/* Stand up the text and vector renderers. Call once from the sokol init
 * callback (host_window_init, after sg_setup). */
void prompt_setup(void);

/* Draw the centered card (dark rounded box, heavy dashed border, the two
 * message lines in the border color) into the current swapchain pass. Pairs
 * with host_window_menu_active() being true, which suppresses the canvas. */
void prompt_draw(const char *line1, const char *line2);

/* True when a framebuffer-pixel point is over the documentation link, which is
 * a real box only while the prompt is the active overlay. */
bool prompt_url_hit(float x, float y);

/* Open that link in the user's browser. */
void prompt_url_open(void);

#endif /* _HOST_SOKOL_APP_PROMPT_H_ */
