/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_WEB_AUDIO_H_
#define _HOST_WEB_AUDIO_H_

/* The web player's sound (audio.c). Set up once the app is, and given the
 * frames' sound after each display callback that ran the machine. While the
 * page is away the machine does not run, nothing is given, and the sound
 * runs dry and goes quiet. */
void web_audio_setup(void);
void web_audio_frame(void);

#endif /* _HOST_WEB_AUDIO_H_ */
