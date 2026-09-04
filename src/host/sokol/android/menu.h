/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The ROM browser, for the entry beside it. A phone has no command line and
 * nothing to drag onto the window, so this list is how a program starts here.
 */

#ifndef _HOST_SOKOL_ANDROID_MENU_H_
#define _HOST_SOKOL_ANDROID_MENU_H_

#include <stdbool.h>

/* The text overlay it draws in. From host_window_init, after sg_setup. */
void menu_setup(void);

/* Find the ROM folder and make it the working directory. */
void menu_chdir(void);

/* Scan the card and put the list up. */
void menu_open(void);

/* Whether it is up: while it is, the machine is held and the canvas hidden. */
bool menu_active(void);

/* Draw it into the current swapchain pass. */
void menu_draw(void);

/* Offer a key to the menu. True when the menu took it — which is every key
 * while it is up, so nothing reaches the machine behind it. */
bool menu_key(int key_code, bool down);

/* Navigation from a stick or hat, while the menu is up. */
void menu_stick(float hat_y, float stick_y);

/* Ask Android for all-files access, then re-read the folder. */
void menu_request_permission(void);
void menu_scan(void);

#endif /* _HOST_SOKOL_ANDROID_MENU_H_ */
