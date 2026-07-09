/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_APP_INPUT_H_
#define _EMU_APP_INPUT_H_

#ifdef __cplusplus
extern "C"
{
#endif

struct sapp_event;

/* Translate one host (sokol) input event into emulated keyboard/mouse input. */
void input_event(const struct sapp_event *e);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_APP_INPUT_H_ */
