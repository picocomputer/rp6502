/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The card a desktop shows when it has no ROM, and the waiting it stands for.
 * Its frame and text are drawn rather than shipped as an image, so they are
 * sharp at any window size; the icon on it is the 64x64 from icon.c. This is
 * where the desktops implement entry_wait_for_rom and the host_window_menu_
 * and host_window_files_dropped functions of entry.h; the web player, which
 * takes no dropped file, has its own in its entry.c.
 */

#ifndef _HOST_SOKOL_APP_PROMPT_H_
#define _HOST_SOKOL_APP_PROMPT_H_

#include <stdbool.h>

struct sapp_event;

/* An event while the card is up: the pointer over the documentation link, and
 * a click on it, which opens it in the user's browser and is consumed. */
bool prompt_event(const struct sapp_event *e);

#endif /* _HOST_SOKOL_APP_PROMPT_H_ */
