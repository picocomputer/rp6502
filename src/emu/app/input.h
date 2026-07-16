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

/* Feed a pending clipboard paste into the keyboard ring, bounded by its free
 * space (the ring drops on overflow). Called once per frame by the window core. */
void input_paste_pump(void);

/* Discard any paste still dripping (a new program must not receive it). */
void input_paste_cancel(void);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_APP_INPUT_H_ */
