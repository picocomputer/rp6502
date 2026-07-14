/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Small helpers shared by the Win32 seam implementations (win/{fs,dir,os}.c).
 */

#ifndef _EMU_WIN_WIN_H_
#define _EMU_WIN_WIN_H_

#include <windows.h>

#define WIN_WPATH_MAX 4096

/* Map GetLastError() to errno, then set it. */
void win_set_errno(DWORD e);

/* Rewrite '\\' to '/' in place (guest paths are '/'-separated). */
void win_to_slash(char *p);

#endif /* _EMU_WIN_WIN_H_ */
