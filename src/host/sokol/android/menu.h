/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The ROM browser.
 */

#ifndef _HOST_SOKOL_ANDROID_MENU_H_
#define _HOST_SOKOL_ANDROID_MENU_H_

#include <stdbool.h>

/* Sets up the text overlay the menu draws in, so it must run after sg_setup;
 * host_window_init is where it does. */
void menu_setup(void);

void menu_chdir(void);
void menu_open(void);
bool menu_active(void);

/* Draws into the current swapchain pass. */
void menu_draw(void);

bool menu_key(int key_code, bool down);

void menu_stick(float hat_y, float stick_y);
void menu_request_permission(void);
void menu_scan(void);

#endif /* _HOST_SOKOL_ANDROID_MENU_H_ */
