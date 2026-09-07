/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_SOKOL_CLI_STATE_H_
#define _HOST_SOKOL_CLI_STATE_H_

/* A savestate as a file on this host's disk, for the two things that ask for
 * one: the script's state verbs and the window's save and load keys.
 *
 * What is here rather than in core is everything core cannot reach -- the
 * file, the audio device's thread, and the in-flight file transfer -- so the
 * two callers do not each have to remember the order those come in.
 */

#include <stdbool.h>

/* True on success. On failure *why is a static reason string, never NULL. */
bool state_save_file(const char *path, const char **why);
bool state_load_file(const char *path, const char **why);

/* The file the window's save and load keys use, named for the program and
 * fixed at startup: the machine chdirs the process itself, so a slot named
 * later would land wherever the program has since gone. NULL where no
 * program was named on the command line. */
void state_slot_init(const char *rom);
const char *state_slot(void);

/* Whether an audio device is open on a thread of its own, which is the only
 * case a walk has to hold still for. The window says yes once it has opened
 * one; a headless or muted run leaves it false and the walks never wait. */
void state_audio_is_threaded(bool on);

#endif /* _HOST_SOKOL_CLI_STATE_H_ */
