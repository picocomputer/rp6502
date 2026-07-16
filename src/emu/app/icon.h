/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_APP_ICON_H_
#define _EMU_APP_ICON_H_

#include "sokol_app.h"

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
