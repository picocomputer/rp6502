/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_APP_ICON_H_
#define _EMU_APP_ICON_H_

#include "sokol_app.h"

/* Self-guarded, unlike sibling headers: sokol_app.h's C++ sapp_run overload
 * can't live in a caller's extern "C", so C++ callers can't wrap us. */
#ifdef __cplusplus
extern "C"
{
#endif

/* Window/taskbar icon candidates (web: favicon), for sapp_set_icon. */
const sapp_icon_desc *icon_desc(void);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_APP_ICON_H_ */
