/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_WEB_AUDIO_H_
#define _HOST_WEB_AUDIO_H_

/* The web player's sound (audio.c), made by the worker's machine. Set up as
 * the machine starts, and given the frames' sound after each step. */
void web_audio_setup(void);
void web_audio_frame(void);

#endif /* _HOST_WEB_AUDIO_H_ */
