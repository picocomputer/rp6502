/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_SOKOL_CLI_STATE_H_
#define _HOST_SOKOL_CLI_STATE_H_

/* Savestates as files on this host's disk, for the script's state verbs. What
 * is here rather than in core is what core cannot reach: the file, the audio
 * device's thread and the in-flight file transfer, so that the caller does not
 * have to sequence those itself.
 */

#include <stdbool.h>

/* True on success. On failure *why is a static reason string, never NULL. */
bool state_save_file(const char *path, const char **why);
bool state_load_file(const char *path, const char **why);

/* A savestate file named for the program and fixed at startup, because a
 * program can chdir the host process and a slot named later would land
 * wherever the program has since gone. */
void state_slot_init(const char *rom);
const char *state_slot(void);

/* Whether an audio device is open whose callback a save or a load has to park
 * out of the engines. The window sets it wherever it opened one; a headless or
 * muted run leaves it false and nothing waits. On the web build the callback
 * is the browser's main thread, so the park runs out its bound instead. */
void state_audio_is_threaded(bool on);

#endif /* _HOST_SOKOL_CLI_STATE_H_ */
